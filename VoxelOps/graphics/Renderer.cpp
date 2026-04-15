


#include "Renderer.hpp"
#include "Mesh.hpp"
#include "Shader.hpp"
#include "ChunkManager.hpp"
#include "Frustum.hpp"
#include "ISkyBackend.hpp"
#include "../player/Player.hpp"
#include "../data/GameData.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/common.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr const char* kDepthLinearizeVs = R"(
#version 330 core
out vec2 vUv;
void main()
{
    const vec2 pos[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 p = pos[gl_VertexID];
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

constexpr const char* kDepthLinearizeFs = R"(
#version 330 core
in vec2 vUv;
layout(location = 0) out float oLinearDepthKm;

uniform sampler2D uDepthTex;
uniform mat4 uInvProj;

void main()
{
    float depth = texture(uDepthTex, vUv).r;
    if (depth >= 0.999999)
    {
        oLinearDepthKm = 0.0;
        return;
    }

    vec2 ndcXY = vUv * 2.0 - 1.0;
    float ndcZ = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(ndcXY, ndcZ, 1.0);
    vec4 viewPos = uInvProj * clipPos;
    viewPos /= max(viewPos.w, 1e-6);

    float depthMeters = length(viewPos.xyz);
    oLinearDepthKm = max(depthMeters * 0.001, 0.0);
}
)";

constexpr const char* kSunShadowVs = R"(
#version 430 core
layout(location = 0) in uint inLow;
layout(location = 1) in uint inHigh;

uniform mat4 uModel;
uniform mat4 uLightViewProj;
uniform vec3 uLightDirWs;
uniform float uCasterNormalBiasMeters;
uniform float uCasterLightBiasMeters;

const vec3 kFaceNormals[6] = vec3[6](
    vec3( 1.0, 0.0, 0.0),
    vec3(-1.0, 0.0, 0.0),
    vec3( 0.0, 1.0, 0.0),
    vec3( 0.0,-1.0, 0.0),
    vec3( 0.0, 0.0, 1.0),
    vec3( 0.0, 0.0,-1.0)
);

void main()
{
    uint low = inLow;
    int face = int((low >> 15u) & 7u);
    vec3 local = vec3(
        float((low >> 0u) & 31u),
        float((low >> 5u) & 31u),
        float((low >> 10u) & 31u));

    vec3 localNormal = kFaceNormals[clamp(face, 0, 5)];
    vec4 worldPos = uModel * vec4(local, 1.0);
    vec3 worldNormal = normalize(mat3(uModel) * localNormal);

    vec3 lightDir = uLightDirWs;
    float lightDirLen = length(lightDir);
    if (lightDirLen > 1e-6) {
        lightDir /= lightDirLen;
    }
    else {
        lightDir = normalize(vec3(0.25, 1.0, 0.2));
    }

    float ndotl = dot(worldNormal, lightDir);
    float slopeFactor = 1.0 - clamp(abs(ndotl), 0.0, 1.0);
    float normalSign = (ndotl >= 0.0) ? -1.0 : 1.0;

    // Caster-side bias: move caster away from light and along slope-aware normal.
    worldPos.xyz += worldNormal * (normalSign * uCasterNormalBiasMeters * slopeFactor);
    worldPos.xyz -= lightDir * uCasterLightBiasMeters;

    gl_Position = uLightViewProj * worldPos;
}
)";

constexpr const char* kSunShadowFs = R"(
#version 430 core
void main()
{
    // Depth-only shadow pass. Color output intentionally omitted.
}
)";

constexpr const char* kSunShadowMomentsBlurVs = R"(
#version 330 core
out vec2 vUv;
void main()
{
    const vec2 pos[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 p = pos[gl_VertexID];
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

constexpr const char* kSunShadowMomentsBlurFs = R"(
#version 330 core
in vec2 vUv;
layout(location = 0) out vec4 oMoments;

uniform sampler2D uMomentsTex;
uniform vec2 uTexelSize;
uniform vec2 uDirection;

void main()
{
    const float w0 = 0.2270270270;
    const float w1 = 0.3162162162;
    const float w2 = 0.0702702703;
    const float o1 = 1.3846153846;
    const float o2 = 3.2307692308;

    vec2 dir = uDirection * uTexelSize;
    vec4 accum = texture(uMomentsTex, vUv).rgba * w0;
    accum += texture(uMomentsTex, vUv + dir * o1).rgba * w1;
    accum += texture(uMomentsTex, vUv - dir * o1).rgba * w1;
    accum += texture(uMomentsTex, vUv + dir * o2).rgba * w2;
    accum += texture(uMomentsTex, vUv - dir * o2).rgba * w2;
    oMoments = accum;
}
)";

GLuint compileInlineShader(GLenum shaderType, const char* source, const char* debugName)
{
    GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }

    GLint logLen = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
    std::string log;
    if (logLen > 0) {
        log.resize(static_cast<size_t>(logLen));
        glGetShaderInfoLog(shader, logLen, nullptr, log.data());
    }
    std::cerr << "[Renderer] Failed to compile " << debugName << ":\n" << log << "\n";
    glDeleteShader(shader);
    return 0;
}

GLuint createDepthLinearizeProgram()
{
    GLuint vs = compileInlineShader(GL_VERTEX_SHADER, kDepthLinearizeVs, "depth_linearize_vs");
    if (vs == 0) {
        return 0;
    }
    GLuint fs = compileInlineShader(GL_FRAGMENT_SHADER, kDepthLinearizeFs, "depth_linearize_fs");
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) {
        return program;
    }

    GLint logLen = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
    std::string log;
    if (logLen > 0) {
        log.resize(static_cast<size_t>(logLen));
        glGetProgramInfoLog(program, logLen, nullptr, log.data());
    }
    std::cerr << "[Renderer] Failed to link depth linearize program:\n" << log << "\n";
    glDeleteProgram(program);
    return 0;
}

GLuint createSunShadowProgram()
{
    GLuint vs = compileInlineShader(GL_VERTEX_SHADER, kSunShadowVs, "sun_shadow_vs");
    if (vs == 0) {
        return 0;
    }
    GLuint fs = compileInlineShader(GL_FRAGMENT_SHADER, kSunShadowFs, "sun_shadow_fs");
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) {
        return program;
    }

    GLint logLen = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
    std::string log;
    if (logLen > 0) {
        log.resize(static_cast<size_t>(logLen));
        glGetProgramInfoLog(program, logLen, nullptr, log.data());
    }
    std::cerr << "[Renderer] Failed to link sun shadow program:\n" << log << "\n";
    glDeleteProgram(program);
    return 0;
}

GLuint createSunShadowMomentsBlurProgram()
{
    GLuint vs = compileInlineShader(GL_VERTEX_SHADER, kSunShadowMomentsBlurVs, "sun_shadow_moments_blur_vs");
    if (vs == 0) {
        return 0;
    }
    GLuint fs = compileInlineShader(GL_FRAGMENT_SHADER, kSunShadowMomentsBlurFs, "sun_shadow_moments_blur_fs");
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) {
        return program;
    }

    GLint logLen = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
    std::string log;
    if (logLen > 0) {
        log.resize(static_cast<size_t>(logLen));
        glGetProgramInfoLog(program, logLen, nullptr, log.data());
    }
    std::cerr << "[Renderer] Failed to link sun shadow moments blur program:\n" << log << "\n";
    glDeleteProgram(program);
    return 0;
}

glm::mat4 makePbrToVoxelAxisSwapMatrix()
{
    // pbr(x,y,z) -> voxel(x,z,y)
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    m[1] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    m[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    m[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return m;
}

glm::mat4 makePbrKmToVoxelMetersMatrix()
{
    const glm::mat4 axisSwap = makePbrToVoxelAxisSwapMatrix();
    const glm::mat4 kilometersToMeters = glm::scale(glm::mat4(1.0f), glm::vec3(1000.0f));
    return kilometersToMeters * axisSwap;
}

glm::vec3 safeNormalizeOrDefault(const glm::vec3& v, const glm::vec3& defaultDir)
{
    const float lenSq = glm::dot(v, v);
    if (lenSq <= 1e-8f) {
        return defaultDir;
    }
    return glm::normalize(v);
}

glm::mat4 buildLightViewProjForScene(
    const glm::vec3& center,
    const glm::vec3& sunDir,
    float halfExtentXZ,
    float halfExtentY,
    int shadowMapSize)
{
    const glm::vec3 lightDirToSun = safeNormalizeOrDefault(sunDir, glm::normalize(glm::vec3(0.25f, 1.0f, 0.2f)));
    const float lightDistance = halfExtentXZ + halfExtentY + 128.0f;
    const glm::vec3 eye = center + lightDirToSun * lightDistance;

    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(up, lightDirToSun)) > 0.96f) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    const glm::mat4 lightView = glm::lookAt(eye, center, up);

    std::array<glm::vec3, 8> corners = {
        center + glm::vec3(-halfExtentXZ, -halfExtentY, -halfExtentXZ),
        center + glm::vec3( halfExtentXZ, -halfExtentY, -halfExtentXZ),
        center + glm::vec3(-halfExtentXZ,  halfExtentY, -halfExtentXZ),
        center + glm::vec3( halfExtentXZ,  halfExtentY, -halfExtentXZ),
        center + glm::vec3(-halfExtentXZ, -halfExtentY,  halfExtentXZ),
        center + glm::vec3( halfExtentXZ, -halfExtentY,  halfExtentXZ),
        center + glm::vec3(-halfExtentXZ,  halfExtentY,  halfExtentXZ),
        center + glm::vec3( halfExtentXZ,  halfExtentY,  halfExtentXZ)
    };

    glm::vec3 minLs(std::numeric_limits<float>::max());
    glm::vec3 maxLs(-std::numeric_limits<float>::max());
    for (const glm::vec3& c : corners) {
        const glm::vec3 ls = glm::vec3(lightView * glm::vec4(c, 1.0f));
        minLs = glm::min(minLs, ls);
        maxLs = glm::max(maxLs, ls);
    }

    // Keep light depth range tight and stable.
    const float zPadding = 32.0f;
    const float nearPlane = std::max(0.1f, -maxLs.z - zPadding);
    const float farPlane = std::max(nearPlane + 1.0f, -minLs.z + zPadding);
    glm::mat4 lightProj = glm::ortho(minLs.x, maxLs.x, minLs.y, maxLs.y, nearPlane, farPlane);

    // Stabilize directional shadow map translation by snapping to texel increments in clip space.
    if (shadowMapSize > 0) {
        const glm::mat4 lightViewProj = lightProj * lightView;
        glm::vec4 shadowOrigin = lightViewProj * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        shadowOrigin *= static_cast<float>(shadowMapSize) * 0.5f;
        const glm::vec4 roundedOrigin = glm::round(shadowOrigin);
        const glm::vec4 roundOffset =
            (roundedOrigin - shadowOrigin) * (2.0f / static_cast<float>(shadowMapSize));
        lightProj[3][0] += roundOffset.x;
        lightProj[3][1] += roundOffset.y;
    }

    return lightProj * lightView;
}

void applyOpaqueWorldState()
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
}

void drainPendingRendererGlErrors(const char* stageTag)
{
    unsigned int firstError = GL_NO_ERROR;
    int count = 0;
    for (;;) {
        const unsigned int err = glGetError();
        if (err == GL_NO_ERROR) {
            break;
        }
        if (count == 0) {
            firstError = err;
        }
        ++count;
    }

    if (count > 0) {
        static int loggedCount = 0;
        if (loggedCount < 16) {
            std::cerr << "[Renderer] Cleared " << count << " pending GL error(s) after " << stageTag
                      << " (first=0x" << std::hex << firstError << std::dec << ").\n";
            ++loggedCount;
        }
    }
}
}

GLuint Renderer::loadTexture(const char* path) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);
    if (!data) { std::cerr << "Failed to load texture: " << path << std::endl; return 0; }

    GLenum internalFormat, format;
    if (nrChannels == 1) {
        internalFormat = GL_R8;
        format = GL_RED;
    }
    else if (nrChannels == 3) {
        internalFormat = GL_SRGB8;      // sRGB internal for correct sampling -> linear data in shader
        format = GL_RGB;
    }
    else if (nrChannels == 4) {
        internalFormat = GL_SRGB8_ALPHA8;
        format = GL_RGBA;
    }
    else {
        std::cerr << "Unsupported channel count: " << nrChannels << std::endl;
        stbi_image_free(data);
        return 0;
    }

    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);


    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    //glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    GLfloat aniso = 2.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);

    glEnable(GL_FRAMEBUFFER_SRGB);

    stbi_image_free(data);

    glBindTexture(GL_TEXTURE_2D, 0);
    return textureID;
}

const Backend& Renderer::getBackend() const noexcept
{
    return m_ActiveBackend;
}

GraphicsBackend Renderer::getActiveBackend() const noexcept
{
    return m_ActiveBackend.getActiveBackend();
}

std::string_view Renderer::getActiveBackendName() const noexcept
{
    return m_ActiveBackend.getActiveBackendName();
}

bool Renderer::isMDIUsable() const noexcept
{
    return m_ActiveBackend.isMDIUsable();
}

void Renderer::shutdown()
{
    releaseSceneCaptureResources();
    releaseSunShadowResources();
}

bool Renderer::ensureDepthLinearizeProgram()
{
    if (m_DepthLinearizeProgram != 0 && m_FullscreenTriangleVao != 0) {
        return true;
    }

    if (m_FullscreenTriangleVao == 0) {
        glGenVertexArrays(1, &m_FullscreenTriangleVao);
    }
    if (m_FullscreenTriangleVao == 0) {
        std::cerr << "[Renderer] Failed to create fullscreen VAO for depth linearization.\n";
        return false;
    }

    m_DepthLinearizeProgram = createDepthLinearizeProgram();
    if (m_DepthLinearizeProgram == 0) {
        std::cerr << "[Renderer] Failed to create depth linearize shader program for scene capture.\n";
        return false;
    }
    return true;
}

bool Renderer::ensureSceneCaptureResources(int width, int height)
{
    if (width <= 0 || height <= 0) {
        static bool loggedInvalidSceneSize = false;
        if (!loggedInvalidSceneSize) {
            std::cerr << "[Renderer] Scene capture skipped: invalid size " << width << "x" << height << ".\n";
            loggedInvalidSceneSize = true;
        }
        return false;
    }

    if (m_SceneFbo != 0 &&
        m_SceneColorTex != 0 &&
        m_SceneDepthTex != 0 &&
        m_SceneLinearDepthTex != 0 &&
        m_SceneWidth == width &&
        m_SceneHeight == height &&
        ensureDepthLinearizeProgram()) {
        return true;
    }

    releaseSceneCaptureResources();
    if (!ensureDepthLinearizeProgram()) {
        std::cerr << "[Renderer] Scene capture unavailable: depth linearization program not ready.\n";
        return false;
    }

    glGenFramebuffers(1, &m_SceneFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFbo);

    glGenTextures(1, &m_SceneColorTex);
    glBindTexture(GL_TEXTURE_2D, m_SceneColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SceneColorTex, 0);

    glGenTextures(1, &m_SceneLinearDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_SceneLinearDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_SceneLinearDepthTex, 0);

    glGenTextures(1, &m_SceneDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_SceneDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_SceneDepthTex, 0);

    const GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBuffers);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Renderer] Scene capture framebuffer incomplete: 0x" << std::hex << status << std::dec
                  << " (size=" << width << "x" << height << ", color=RGBA16F, linearDepth=R32F, depth=D32F).\n";
        releaseSceneCaptureResources();
        return false;
    }

    m_SceneWidth = width;
    m_SceneHeight = height;
    return true;
}

void Renderer::releaseSceneCaptureResources()
{
    if (m_SceneDepthTex != 0) {
        glDeleteTextures(1, &m_SceneDepthTex);
        m_SceneDepthTex = 0;
    }
    if (m_SceneLinearDepthTex != 0) {
        glDeleteTextures(1, &m_SceneLinearDepthTex);
        m_SceneLinearDepthTex = 0;
    }
    if (m_SceneColorTex != 0) {
        glDeleteTextures(1, &m_SceneColorTex);
        m_SceneColorTex = 0;
    }
    if (m_SceneFbo != 0) {
        glDeleteFramebuffers(1, &m_SceneFbo);
        m_SceneFbo = 0;
    }
    if (m_DepthLinearizeProgram != 0) {
        glDeleteProgram(m_DepthLinearizeProgram);
        m_DepthLinearizeProgram = 0;
    }
    if (m_FullscreenTriangleVao != 0) {
        glDeleteVertexArrays(1, &m_FullscreenTriangleVao);
        m_FullscreenTriangleVao = 0;
    }
    m_SceneWidth = 0;
    m_SceneHeight = 0;
}

bool Renderer::ensureSunShadowResources()
{
    bool hasAllShadowResources = (m_SunShadowProgram != 0) && (m_SunShadowMomentsBlurProgram != 0);
    for (int i = 0; i < kSunShadowCascadeCount; ++i) {
        hasAllShadowResources = hasAllShadowResources && (m_SunShadowFbo[i] != 0) && (m_SunShadowDepthTex[i] != 0);
    }
    hasAllShadowResources =
        hasAllShadowResources &&
        (m_SunShadowMomentsTex != 0) &&
        (m_SunShadowMomentsTempTex != 0) &&
        (m_SunShadowMomentsFbo != 0);
    if (hasAllShadowResources) {
        return true;
    }

    releaseSunShadowResources();

    m_SunShadowProgram = createSunShadowProgram();
    if (m_SunShadowProgram == 0) {
        return false;
    }
    m_SunShadowMomentsBlurProgram = createSunShadowMomentsBlurProgram();
    if (m_SunShadowMomentsBlurProgram == 0) {
        releaseSunShadowResources();
        return false;
    }
    if (m_FullscreenTriangleVao == 0) {
        glGenVertexArrays(1, &m_FullscreenTriangleVao);
        if (m_FullscreenTriangleVao == 0) {
            std::cerr << "[Renderer] Failed to allocate fullscreen VAO for EVSM blur.\n";
            releaseSunShadowResources();
            return false;
        }
    }

    glGenTextures(1, &m_SunShadowMomentsTex);
    glGenTextures(1, &m_SunShadowMomentsTempTex);
    glGenFramebuffers(1, &m_SunShadowMomentsFbo);
    if (m_SunShadowMomentsTex == 0 || m_SunShadowMomentsTempTex == 0 || m_SunShadowMomentsFbo == 0) {
        std::cerr << "[Renderer] Failed to allocate EVSM moments resources.\n";
        releaseSunShadowResources();
        return false;
    }

    auto allocateMomentsTex = [&](GLuint tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            // EVSM4 moments in 32-bit float to avoid half-float quantization striping.
            GL_RGBA32F,
            m_SunShadowMapSize,
            m_SunShadowMapSize,
            0,
            GL_RGBA,
            GL_FLOAT,
            nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    allocateMomentsTex(m_SunShadowMomentsTex);
    allocateMomentsTex(m_SunShadowMomentsTempTex);

    glGenTextures(kSunShadowCascadeCount, m_SunShadowDepthTex.data());
    glGenFramebuffers(kSunShadowCascadeCount, m_SunShadowFbo.data());

    for (int i = 0; i < kSunShadowCascadeCount; ++i) {
        if (m_SunShadowDepthTex[i] == 0 || m_SunShadowFbo[i] == 0) {
            std::cerr << "[Renderer] Failed to allocate CSM resources for cascade " << i << ".\n";
            releaseSunShadowResources();
            return false;
        }

        glBindTexture(GL_TEXTURE_2D, m_SunShadowDepthTex[i]);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_DEPTH_COMPONENT32F,
            m_SunShadowMapSize,
            m_SunShadowMapSize,
            0,
            GL_DEPTH_COMPONENT,
            GL_FLOAT,
            nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float borderColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

        glBindFramebuffer(GL_FRAMEBUFFER, m_SunShadowFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_SunShadowDepthTex[i], 0);
        if (i == 0) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SunShadowMomentsTex, 0);
            const GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBuffers);
        }
        else {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glDrawBuffer(GL_NONE);
        }
        glReadBuffer(GL_NONE);

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[Renderer] Sun shadow framebuffer incomplete for cascade " << i
                      << ": 0x" << std::hex << status << std::dec << "\n";
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            releaseSunShadowResources();
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

void Renderer::releaseSunShadowResources()
{
    if (m_SunShadowMomentsFbo != 0) {
        glDeleteFramebuffers(1, &m_SunShadowMomentsFbo);
        m_SunShadowMomentsFbo = 0;
    }
    if (m_SunShadowMomentsTex != 0) {
        glDeleteTextures(1, &m_SunShadowMomentsTex);
        m_SunShadowMomentsTex = 0;
    }
    if (m_SunShadowMomentsTempTex != 0) {
        glDeleteTextures(1, &m_SunShadowMomentsTempTex);
        m_SunShadowMomentsTempTex = 0;
    }
    for (int i = 0; i < kSunShadowCascadeCount; ++i) {
        if (m_SunShadowFbo[i] != 0) {
            glDeleteFramebuffers(1, &m_SunShadowFbo[i]);
            m_SunShadowFbo[i] = 0;
        }
    }
    for (int i = 0; i < kSunShadowCascadeCount; ++i) {
        if (m_SunShadowDepthTex[i] != 0) {
            glDeleteTextures(1, &m_SunShadowDepthTex[i]);
            m_SunShadowDepthTex[i] = 0;
        }
        m_LastSunShadowViewProjVoxel[i] = glm::mat4(1.0f);
        m_SunShadowCascadeFarMeters[i] = 0.0f;
    }
    if (m_SunShadowProgram != 0) {
        glDeleteProgram(m_SunShadowProgram);
        m_SunShadowProgram = 0;
    }
    if (m_SunShadowMomentsBlurProgram != 0) {
        glDeleteProgram(m_SunShadowMomentsBlurProgram);
        m_SunShadowMomentsBlurProgram = 0;
    }
}

bool Renderer::runSunShadowMomentsBlurPass()
{
    if (m_FullscreenTriangleVao == 0) {
        glGenVertexArrays(1, &m_FullscreenTriangleVao);
    }
    if (m_SunShadowMomentsBlurProgram == 0 || m_SunShadowMomentsTex == 0 || m_SunShadowMomentsTempTex == 0 || m_SunShadowMomentsFbo == 0 || m_FullscreenTriangleVao == 0) {
        static bool loggedMissingMomentsBlurResources = false;
        if (!loggedMissingMomentsBlurResources) {
            std::cerr << "[Renderer] EVSM blur resources missing (program/tex/fbo/vao).\n";
            loggedMissingMomentsBlurResources = true;
        }
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_SunShadowMomentsFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SunShadowMomentsTempTex, 0);
    const GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBuffers);
    glReadBuffer(GL_NONE);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        static bool loggedMomentsFboStatus = false;
        if (!loggedMomentsFboStatus) {
            std::cerr << "[Renderer] EVSM blur framebuffer incomplete: 0x"
                      << std::hex << status << std::dec << ".\n";
            loggedMomentsFboStatus = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glViewport(0, 0, m_SunShadowMapSize, m_SunShadowMapSize);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(m_SunShadowMomentsBlurProgram);
    const GLint texLoc = glGetUniformLocation(m_SunShadowMomentsBlurProgram, "uMomentsTex");
    const GLint texelSizeLoc = glGetUniformLocation(m_SunShadowMomentsBlurProgram, "uTexelSize");
    const GLint directionLoc = glGetUniformLocation(m_SunShadowMomentsBlurProgram, "uDirection");
    if (texLoc >= 0) {
        glUniform1i(texLoc, 0);
    }
    if (texelSizeLoc >= 0) {
        const float texelSize = 1.0f / static_cast<float>(std::max(1, m_SunShadowMapSize));
        glUniform2f(texelSizeLoc, texelSize, texelSize);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_FullscreenTriangleVao);

    if (directionLoc >= 0) {
        glUniform2f(directionLoc, 1.0f, 0.0f);
    }
    glBindTexture(GL_TEXTURE_2D, m_SunShadowMomentsTex);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SunShadowMomentsTex, 0);
    if (directionLoc >= 0) {
        glUniform2f(directionLoc, 0.0f, 1.0f);
    }
    glBindTexture(GL_TEXTURE_2D, m_SunShadowMomentsTempTex);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDepthMask(GL_TRUE);
    drainPendingRendererGlErrors("EVSM near blur pass");
    return true;
}

bool Renderer::renderSunShadowMaps(
    RenderFrameParams& params,
    const glm::mat4& projection,
    const glm::mat4& view,
    glm::mat4& outShadowViewProj)
{
    auto logSunShadowStageError = [](const char* stage, int cascadeIndex) {
        unsigned int firstError = GL_NO_ERROR;
        int count = 0;
        for (;;) {
            const unsigned int err = glGetError();
            if (err == GL_NO_ERROR) {
                break;
            }
            if (count == 0) {
                firstError = err;
            }
            ++count;
        }

        if (count > 0) {
            static int loggedCount = 0;
            if (loggedCount < 64) {
                std::cerr << "[Renderer][SunShadow] GL error after " << stage;
                if (cascadeIndex >= 0) {
                    std::cerr << " (cascade=" << cascadeIndex << ")";
                }
                std::cerr << ": count=" << count
                          << " first=0x" << std::hex << firstError << std::dec << "\n";
                ++loggedCount;
            }
        }
    };

    // Terrain path currently uses precise depth-compare CSM; keep EVSM moments disabled.
    const bool useNearEvsmMoments = false;

    (void)projection;
    (void)view;
    if (!ensureSunShadowResources()) {
        return false;
    }
    logSunShadowStageError("ensureSunShadowResources", -1);

    const float maxShadowDistanceMeters = std::max(128.0f, static_cast<float>(params.player.renderDistance * CHUNK_SIZE));
    // Give more resolution to cascade 0 so side-face shadow edges quantize less.
    const float cascadeFractions[kSunShadowCascadeCount] = { 0.24f, 1.0f };
    const glm::vec3 baseShadowCenter = params.activeCamera.position;
    const glm::vec3 sunDir = params.sky.getSunDir();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(m_SunShadowProgram);
    logSunShadowStageError("glUseProgram(shadow)", -1);
    const GLint shadowLightDirLoc = glGetUniformLocation(m_SunShadowProgram, "uLightDirWs");
    const GLint shadowCasterNormalBiasLoc = glGetUniformLocation(m_SunShadowProgram, "uCasterNormalBiasMeters");
    const GLint shadowCasterLightBiasLoc = glGetUniformLocation(m_SunShadowProgram, "uCasterLightBiasMeters");
    const GLint shadowWriteMomentsLoc = glGetUniformLocation(m_SunShadowProgram, "uWriteMoments");
    const GLint shadowEvsmPosExponentLoc = glGetUniformLocation(m_SunShadowProgram, "uEvsmPosExponent");
    const GLint shadowEvsmNegExponentLoc = glGetUniformLocation(m_SunShadowProgram, "uEvsmNegExponent");

    const glm::vec3 sunDirWs = safeNormalizeOrDefault(sunDir, glm::normalize(glm::vec3(0.25f, 1.0f, 0.2f)));
    if (shadowLightDirLoc >= 0) {
        glUniform3fv(shadowLightDirLoc, 1, glm::value_ptr(sunDirWs));
    }

    const float sunGrazing = 1.0f - glm::clamp(std::abs(sunDirWs.y), 0.0f, 1.0f);
    const float lowSunBoost = 1.0f +
        sunGrazing * sunGrazing * glm::clamp(params.sunShadowLowSunBiasBoost, 0.0f, 4.0f);
    const bool useTwoSidedShadowCastersForLowSun =
        params.sunShadowFrontFaceCullAtLowSun &&
        (sunGrazing >= glm::clamp(params.sunShadowFrontFaceCullGrazingThreshold, 0.0f, 1.0f));
    if (useTwoSidedShadowCastersForLowSun) {
        glDisable(GL_CULL_FACE);
    }
    else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    const float casterNormalBiasBase[kSunShadowCascadeCount] = { 0.00065f, 0.00105f };
    const float casterLightBiasBase[kSunShadowCascadeCount] = { 0.0018f, 0.0030f };
    const float evsmPosExponent = 4.2f;
    const float evsmNegExponent = 4.2f;

    float previousCascadeFarMeters = 0.0f;
    for (int cascadeIndex = 0; cascadeIndex < kSunShadowCascadeCount; ++cascadeIndex) {
        // Depth pass helpers may bind/unbind programs between cascades.
        // Rebind explicitly so per-cascade glUniform calls are always valid.
        glUseProgram(m_SunShadowProgram);

        float cascadeFarMeters = maxShadowDistanceMeters * cascadeFractions[cascadeIndex];
        if (cascadeIndex > 0) {
            cascadeFarMeters = std::max(cascadeFarMeters, previousCascadeFarMeters + 64.0f);
        }
        previousCascadeFarMeters = cascadeFarMeters;
        m_SunShadowCascadeFarMeters[cascadeIndex] = cascadeFarMeters;

        const float worldRadiusXZ = (cascadeIndex == 0)
            ? std::max(40.0f, std::min(cascadeFarMeters * 0.52f, 96.0f))
            : std::max(96.0f, cascadeFarMeters * 1.10f);
        const float worldRadiusY = (cascadeIndex == 0) ? 80.0f : 192.0f;

        glm::vec3 shadowCenter = baseShadowCenter;
        // Ignore head-bob/small movement to keep sun-shadow projection stable.
        const float centerSnap = (cascadeIndex == 0) ? 2.0f : 4.0f;
        shadowCenter.x = std::floor(shadowCenter.x / centerSnap) * centerSnap;
        shadowCenter.y = std::floor(shadowCenter.y / 8.0f) * 8.0f;
        shadowCenter.z = std::floor(shadowCenter.z / centerSnap) * centerSnap;

        const glm::mat4 lightViewProjVoxel = buildLightViewProjForScene(
            shadowCenter,
            sunDir,
            worldRadiusXZ,
            worldRadiusY,
            m_SunShadowMapSize);
        m_LastSunShadowViewProjVoxel[cascadeIndex] = lightViewProjVoxel;

        glBindFramebuffer(GL_FRAMEBUFFER, m_SunShadowFbo[cascadeIndex]);
        glViewport(0, 0, m_SunShadowMapSize, m_SunShadowMapSize);
        if (cascadeIndex == 0 && useNearEvsmMoments) {
            const GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBuffers);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            if (shadowWriteMomentsLoc >= 0) {
                glUniform1i(shadowWriteMomentsLoc, 1);
            }
            if (shadowEvsmPosExponentLoc >= 0) {
                glUniform1f(shadowEvsmPosExponentLoc, evsmPosExponent);
            }
            if (shadowEvsmNegExponentLoc >= 0) {
                glUniform1f(shadowEvsmNegExponentLoc, evsmNegExponent);
            }
            const float clearPos = std::exp(evsmPosExponent);
            const float clearNeg = -std::exp(-evsmNegExponent);
            const float clearMoments[4] = {
                clearPos,
                clearPos * clearPos,
                clearNeg,
                clearNeg * clearNeg
            };
            glClearBufferfv(GL_COLOR, 0, clearMoments);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        else {
            // Depth-only draw state for cascade FBOs is configured at creation time.
            // Avoid redundant per-frame glDrawBuffer calls on some drivers.
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            if (shadowWriteMomentsLoc >= 0) {
                glUniform1i(shadowWriteMomentsLoc, 0);
            }
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        if (shadowCasterNormalBiasLoc >= 0) {
            glUniform1f(shadowCasterNormalBiasLoc, casterNormalBiasBase[cascadeIndex] * lowSunBoost);
        }
        if (shadowCasterLightBiasLoc >= 0) {
            glUniform1f(shadowCasterLightBiasLoc, casterLightBiasBase[cascadeIndex] * lowSunBoost);
        }
        logSunShadowStageError("cascade setup", cascadeIndex);
        glEnable(GL_POLYGON_OFFSET_FILL);
        const float polygonUnitsScale = glm::mix(1.0f, lowSunBoost, 0.78f);
        if (cascadeIndex == 0) {
            glPolygonOffset(0.0f, 0.30f * polygonUnitsScale);
        }
        else {
            glPolygonOffset(0.0f, 0.44f * polygonUnitsScale);
        }

        const int cascadeRenderDistance = std::max(
            2,
            static_cast<int>(std::ceil(cascadeFarMeters / static_cast<float>(CHUNK_SIZE))) + 2);
        params.chunkManager.renderChunksDepthPass(
            m_SunShadowProgram,
            lightViewProjVoxel,
            shadowCenter,
            cascadeRenderDistance);
        logSunShadowStageError("renderChunksDepthPass", cascadeIndex);
    }

    glUseProgram(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    if (useNearEvsmMoments) {
        if (!runSunShadowMomentsBlurPass()) {
            static bool loggedMomentsBlurFailure = false;
            if (!loggedMomentsBlurFailure) {
                std::cerr << "[Renderer] EVSM near blur pass unavailable; keeping unblurred near moments this frame.\n";
                loggedMomentsBlurFailure = true;
            }
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    logSunShadowStageError("finalize", -1);
    drainPendingRendererGlErrors("sun shadow map rendering");

    // PbrSkyLib world is kilometers + Z-up, while VoxelOps shadow map world is meters + Y-up.
    // Convert PBR-space positions to voxel shadow-map space before depth-compare sampling.
    outShadowViewProj = m_LastSunShadowViewProjVoxel[0] * makePbrKmToVoxelMetersMatrix();
    return true;
}

void Renderer::runDepthLinearizePass(const glm::mat4& projection)
{
    if (m_SceneFbo == 0 || m_SceneDepthTex == 0 || m_SceneLinearDepthTex == 0 || m_DepthLinearizeProgram == 0) {
        return;
    }

    // Avoid undefined feedback: do not sample a texture while it remains attached to the bound FBO.
    glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

    const GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(1, drawBuffers);
    glViewport(0, 0, m_SceneWidth, m_SceneHeight);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glUseProgram(m_DepthLinearizeProgram);
    const glm::mat4 invProj = glm::inverse(projection);
    const GLint invProjLoc = glGetUniformLocation(m_DepthLinearizeProgram, "uInvProj");
    if (invProjLoc >= 0) {
        glUniformMatrix4fv(invProjLoc, 1, GL_FALSE, glm::value_ptr(invProj));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_SceneDepthTex);
    const GLint depthTexLoc = glGetUniformLocation(m_DepthLinearizeProgram, "uDepthTex");
    if (depthTexLoc >= 0) {
        glUniform1i(depthTexLoc, 0);
    }

    glBindVertexArray(m_FullscreenTriangleVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    // Restore scene depth attachment for next frame's world rendering.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_SceneDepthTex, 0);

    const GLenum sceneColorBuffer[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, sceneColorBuffer);
    glDepthMask(GL_TRUE);
}


void Renderer::beginFrame()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::endFrame()
{
    // swap buffers (outside if handled elsewhere)
}

void Renderer::renderFrame(RenderFrameParams& params)
{
    drainPendingRendererGlErrors("frame start");
    const Camera& cullingCamera = params.cullingCamera ? *params.cullingCamera : params.activeCamera;

    int frameWidth = GameData::screenWidth;
    int frameHeight = GameData::screenHeight;
    if (frameWidth <= 0 || frameHeight <= 0) {
        GLint viewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, viewport);
        frameWidth = viewport[2];
        frameHeight = viewport[3];
    }
    if (frameWidth <= 0 || frameHeight <= 0) {
        static bool loggedInvalidFrameSize = false;
        if (!loggedInvalidFrameSize) {
            std::cerr << "[Renderer] Skipping frame: invalid framebuffer size " << frameWidth << "x" << frameHeight << ".\n";
            loggedInvalidFrameSize = true;
        }
        return;
    }

    glm::mat4 projection = glm::perspective(
        glm::radians(GameData::FOV),
        static_cast<float>(frameWidth) / static_cast<float>(frameHeight),
        0.1f,
        100000.0f
    );
    glm::mat4 view = params.activeCamera.getViewMatrix();
    params.sky.setViewFovYDegrees(GameData::FOV);

    bool useExternalSceneTextures = params.sky.requiresExternalSceneTextures();
    if (useExternalSceneTextures) {
        useExternalSceneTextures = ensureSceneCaptureResources(frameWidth, frameHeight);
        if (!useExternalSceneTextures) {
            static bool loggedSceneCaptureFailure = false;
            if (!loggedSceneCaptureFailure) {
                std::cerr << "[Renderer] Falling back to direct sky path: failed to allocate scene capture resources.\n";
                loggedSceneCaptureFailure = true;
            }
        }
    }

    bool useExternalShadowMap = false;
    glm::mat4 externalShadowViewProj(1.0f);
    if (useExternalSceneTextures && params.sky.supportsExternalShadowMap()) {
        useExternalShadowMap = renderSunShadowMaps(params, projection, view, externalShadowViewProj);
        drainPendingRendererGlErrors("sun shadow maps");
        if (!useExternalShadowMap) {
            static bool loggedShadowFailure = false;
            if (!loggedShadowFailure) {
                std::cerr << "[Renderer] Realistic sky fallback: failed to render external sun shadow map.\n";
                loggedShadowFailure = true;
            }
        }
    }

    if (useExternalSceneTextures) {
        static bool loggedSceneCaptureReady = false;
        if (!loggedSceneCaptureReady) {
            std::cerr << "[Renderer] Scene capture ready: " << m_SceneWidth << "x" << m_SceneHeight << ".\n";
            loggedSceneCaptureReady = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_SceneFbo);
        const GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
        glViewport(0, 0, m_SceneWidth, m_SceneHeight);
    }
    else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, frameWidth, frameHeight);
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    beginFrame();
    glEnable(GL_MULTISAMPLE);

    if (!useExternalSceneTextures) {
        params.sky.setCameraFromActiveCamera(params.activeCamera);
        const bool framebufferSrgbWasEnabled = (glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE);
        if (params.sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        params.sky.render(projection, view);
        if (params.sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
    }

    // Backends may alter global GL state during sky/fullscreen passes.
    // Re-apply world-pass state to keep chunk rendering deterministic.
    applyOpaqueWorldState();

    // World pass
    const glm::mat4 viewProjection = projection * view;
    const glm::mat4 cullingViewProjection = projection * cullingCamera.getViewMatrix();
    params.frustum.extractPlanes(cullingViewProjection);

    const glm::vec3 lightDir = params.sky.getSunDir();
    const glm::vec3 lightColor = glm::vec3(1.0f, 0.98f, 0.96f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, params.chunkManager.atlas.atlasTextureArrayID);
    drainPendingRendererGlErrors("chunk pass bind atlas");

    params.chunkShader.use();
    drainPendingRendererGlErrors("chunk pass use shader");
    params.chunkShader.setMat4("viewProj", viewProjection);
    params.chunkShader.setInt("uOutputHdrLinear", useExternalSceneTextures ? 1 : 0);
    // Keep terrain slightly darker than sky for better separation.
    params.chunkShader.setFloat("uHdrTerrainExposureScale", 0.92f);
    // Keep physical depth units (km) to match the PbrSky sample integration path.
    params.chunkShader.setFloat("uAerialDepthScaleKm", 1.0f);
    const bool useChunkSunShadow =
        useExternalShadowMap &&
        (m_SunShadowDepthTex[0] != 0) &&
        (m_SunShadowDepthTex[1] != 0);
    params.chunkShader.setInt("uUseSunShadowMap", useChunkSunShadow ? 1 : 0);
    // Keep sampler bindings deterministic to avoid driver draw-time sampler-type conflicts.
    params.chunkShader.setInt("texture1", 0);
    params.chunkShader.setInt("uSunShadowTexNear", 1);
    params.chunkShader.setInt("uSunShadowTexFar", 2);
    params.chunkShader.setInt("uSunShadowMomentsNear", 3);
    params.chunkShader.setFloat(
        "uSunShadowLowSunBiasBoost",
        glm::clamp(params.sunShadowLowSunBiasBoost, 0.0f, 4.0f));
    // Realistic path disables baked channel when dynamic sun shadows are active.
    // Fall back to baked only when dynamic map is unavailable to avoid totally flat lighting.
    const bool useBakedSunChannel = !params.sky.requiresExternalSceneTextures() || !useChunkSunShadow;
    params.chunkShader.setInt("uUseBakedSunChannel", useBakedSunChannel ? 1 : 0);
    if (useChunkSunShadow) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_SunShadowDepthTex[0]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_SunShadowDepthTex[1]);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_SunShadowMomentsTex);
        // Keep terrain on precise depth-compare CSM. EVSM moments remain available for other paths.
        params.chunkShader.setInt("uUseSunShadowMomentsNear", 0);
        params.chunkShader.setMat4("uSunShadowViewProjNear", m_LastSunShadowViewProjVoxel[0]);
        params.chunkShader.setMat4("uSunShadowViewProjFar", m_LastSunShadowViewProjVoxel[1]);
        const float texelSize = 1.0f / static_cast<float>(std::max(1, m_SunShadowMapSize));
        params.chunkShader.setVec2("uSunShadowTexelSizeNear", glm::vec2(texelSize));
        params.chunkShader.setVec2("uSunShadowTexelSizeFar", glm::vec2(texelSize));
        params.chunkShader.setFloat("uSunShadowSplitDepthKm", m_SunShadowCascadeFarMeters[0] * 0.001f);
        params.chunkShader.setFloat("uSunShadowBlendKm", 0.012f);
    }
    else {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        params.chunkShader.setInt("uUseSunShadowMomentsNear", 0);
        params.chunkShader.setVec2("uSunShadowTexelSizeNear", glm::vec2(0.0f));
        params.chunkShader.setVec2("uSunShadowTexelSizeFar", glm::vec2(0.0f));
        params.chunkShader.setFloat("uSunShadowSplitDepthKm", 0.0f);
        params.chunkShader.setFloat("uSunShadowBlendKm", 0.001f);
    }
    params.chunkShader.setVec3("uSunShadowDirectionalBias", params.sunShadowDirectionalBias);
    drainPendingRendererGlErrors("chunk pass uniforms setup");

    bool localUniformState = false;
    bool& uniformsConfigured = params.chunkUniformsInitialized ? *params.chunkUniformsInitialized : localUniformState;
    params.chunkShader.setVec3("lightDir", lightDir);
    params.chunkShader.setVec3("lightColor", lightColor);
    if (!uniformsConfigured) {
        if (params.chunkManager.enableAO) {
            params.chunkShader.setVec3("skyColorTop", glm::vec3(0.58f, 0.73f, 0.95f));
            params.chunkShader.setVec3("skyColorBottom", glm::vec3(0.86f, 0.91f, 0.98f));

            params.chunkShader.setFloat("ambientStrength", 0.89f);
            params.chunkShader.setFloat("diffuseStrength", 0.85f);
            params.chunkShader.setFloat("minAmbient", 0.01f);

            params.chunkShader.setFloat("hemiTint", 0.5f);
            params.chunkShader.setFloat("contrast", 1.0f);
            params.chunkShader.setFloat("satBoost", 1.17f);
            params.chunkShader.setVec3("warmth", glm::vec3(1.03f, 1.00f, 0.97f));

            params.chunkShader.setFloat("aoPow", 0.8f);
            params.chunkShader.setFloat("aoMin", 0.6f);
            params.chunkShader.setFloat("aoApplyAfterTone", 0.8f);

            params.chunkShader.setFloat("shadowDarkness", 0.3f);
            params.chunkShader.setFloat("shadowContrast", 1.15f);
        }

        uniformsConfigured = true;
    }

    // Camera position changes every frame; keep this outside one-time static uniforms.
    params.chunkShader.setVec3("cameraPos", params.activeCamera.position);
    glActiveTexture(GL_TEXTURE0);
    glPolygonMode(GL_FRONT_AND_BACK, params.toggleWireframe ? GL_LINE : GL_FILL);
    drainPendingRendererGlErrors("chunk pass pre-draw");

    params.chunkManager.renderChunks(
        params.chunkShader,
        params.frustum,
        cullingCamera.position,
        params.player.renderDistance
    );
    drainPendingRendererGlErrors("chunk world pass");

    const glm::vec3 ambientColor = glm::vec3(0.36f, 0.40f, 0.46f);
    params.player.renderRemotePlayers(view, projection, lightDir, lightColor, ambientColor);
    drainPendingRendererGlErrors("remote player world pass");

    // Debug passes
    if (params.toggleChunkBorders) {
        params.chunkManager.renderChunkBorders(view, projection);
        drainPendingRendererGlErrors("chunk border pass");
    }

    if (params.toggleDebugFrustum) {
        params.frustum.drawFrustumFaces(
            params.debugShader,
            projection * view,
            view,
            projection,
            params.toggleWireframe
        );
        drainPendingRendererGlErrors("frustum debug pass");
    }

    if (params.renderOpaqueOverlayPasses) {
        params.renderOpaqueOverlayPasses();
        drainPendingRendererGlErrors("opaque overlay pass");
    }

    if (useExternalSceneTextures) {
        // Build linear depth from the resolved scene depth buffer so every opaque pass
        // (chunks + remote players + world items) contributes consistently to AP.
        drainPendingRendererGlErrors("before scene depth linearization");
        runDepthLinearizePass(projection);
        drainPendingRendererGlErrors("scene depth linearization");
    }

    if (useExternalSceneTextures) {
        params.sky.setCameraFromActiveCamera(params.activeCamera);
        const bool framebufferSrgbWasEnabled = (glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE);
        if (params.sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }

        // Scene color + linearized depth feed the external PbrSky composition pass.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, frameWidth, frameHeight);
        params.sky.setExternalSceneTextures(m_SceneColorTex, m_SceneLinearDepthTex);
        if (useExternalShadowMap) {
            params.sky.setExternalShadowMap(m_SunShadowDepthTex[0], externalShadowViewProj);
        }
        else {
            params.sky.clearExternalShadowMap();
        }
        drainPendingRendererGlErrors("external sky input setup");
        params.sky.render(projection, view);
        params.sky.clearExternalSceneTextures();
        params.sky.clearExternalShadowMap();

        if (params.sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
    }

    // Keep downstream passes (held gun, UI overlays) deterministic.
    applyOpaqueWorldState();
}











