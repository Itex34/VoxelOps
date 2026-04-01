


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

void main()
{
    uint low = inLow;
    vec3 local = vec3(
        float((low >> 0u) & 31u),
        float((low >> 5u) & 31u),
        float((low >> 10u) & 31u));
    vec4 worldPos = uModel * vec4(local, 1.0);
    gl_Position = uLightViewProj * worldPos;
}
)";

constexpr const char* kSunShadowFs = R"(
#version 430 core
void main() {}
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

    // Stabilize shadow projection by snapping the ortho window to the shadow-map texel grid.
    if (shadowMapSize > 0) {
        const float width = std::max(maxLs.x - minLs.x, 1e-3f);
        const float height = std::max(maxLs.y - minLs.y, 1e-3f);
        const float texelSizeX = width / static_cast<float>(shadowMapSize);
        const float texelSizeY = height / static_cast<float>(shadowMapSize);

        if (texelSizeX > 0.0f && texelSizeY > 0.0f) {
            const float centerX = 0.5f * (minLs.x + maxLs.x);
            const float centerY = 0.5f * (minLs.y + maxLs.y);
            const float snappedCenterX = std::floor(centerX / texelSizeX + 0.5f) * texelSizeX;
            const float snappedCenterY = std::floor(centerY / texelSizeY + 0.5f) * texelSizeY;
            minLs.x = snappedCenterX - width * 0.5f;
            maxLs.x = snappedCenterX + width * 0.5f;
            minLs.y = snappedCenterY - height * 0.5f;
            maxLs.y = snappedCenterY + height * 0.5f;
        }
    }

    // Keep light depth range stable to avoid frame-to-frame reprojection flicker.
    const float zPadding = 48.0f;
    const float depthExtent = halfExtentXZ + halfExtentY + zPadding;
    const float nearPlane = std::max(0.1f, lightDistance - depthExtent);
    const float farPlane = std::max(nearPlane + 1.0f, lightDistance + depthExtent);
    const glm::mat4 lightProj = glm::ortho(minLs.x, maxLs.x, minLs.y, maxLs.y, nearPlane, farPlane);
    return lightProj * lightView;
}

void applyOpaqueWorldState()
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
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
    if (m_SunShadowFbo != 0 && m_SunShadowDepthTex != 0 && m_SunShadowProgram != 0) {
        return true;
    }

    releaseSunShadowResources();

    m_SunShadowProgram = createSunShadowProgram();
    if (m_SunShadowProgram == 0) {
        return false;
    }

    glGenTextures(1, &m_SunShadowDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_SunShadowDepthTex);
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

    glGenFramebuffers(1, &m_SunShadowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_SunShadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_SunShadowDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Renderer] Sun shadow framebuffer incomplete: 0x" << std::hex << status << std::dec << "\n";
        releaseSunShadowResources();
        return false;
    }

    return true;
}

void Renderer::releaseSunShadowResources()
{
    if (m_SunShadowFbo != 0) {
        glDeleteFramebuffers(1, &m_SunShadowFbo);
        m_SunShadowFbo = 0;
    }
    if (m_SunShadowDepthTex != 0) {
        glDeleteTextures(1, &m_SunShadowDepthTex);
        m_SunShadowDepthTex = 0;
    }
    if (m_SunShadowProgram != 0) {
        glDeleteProgram(m_SunShadowProgram);
        m_SunShadowProgram = 0;
    }
}

bool Renderer::renderSunShadowMap(
    RenderFrameParams& params,
    const glm::mat4& projection,
    const glm::mat4& view,
    glm::mat4& outShadowViewProj)
{
    (void)projection;
    (void)view;
    if (!ensureSunShadowResources()) {
        return false;
    }

    const float worldRadiusXZ = std::max(64.0f, static_cast<float>(params.player.renderDistance * CHUNK_SIZE) * 1.0f);
    const float worldRadiusY = 192.0f;
    glm::vec3 shadowCenter = params.activeCamera.position;
    // Ignore head-bob/small vertical motion to keep sun-shadow projection stable.
    shadowCenter.y = std::floor(shadowCenter.y / 8.0f) * 8.0f;
    const glm::vec3 sunDir = params.sky.getSunDir();

    const glm::mat4 lightViewProjVoxel = buildLightViewProjForScene(
        shadowCenter,
        sunDir,
        worldRadiusXZ,
        worldRadiusY,
        m_SunShadowMapSize);
    m_LastSunShadowViewProjVoxel = lightViewProjVoxel;

    glBindFramebuffer(GL_FRAMEBUFFER, m_SunShadowFbo);
    glViewport(0, 0, m_SunShadowMapSize, m_SunShadowMapSize);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    // Conservative offset to reduce acne without erasing cast shadows.
    glPolygonOffset(0.9f, 1.8f);
    glUseProgram(m_SunShadowProgram);

    params.chunkManager.renderChunksDepthPass(
        m_SunShadowProgram,
        lightViewProjVoxel,
        params.activeCamera.position,
        params.player.renderDistance);

    glUseProgram(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // PbrSkyLib world is kilometers + Z-up, while VoxelOps shadow map world is meters + Y-up.
    // Convert PBR-space positions to voxel shadow-map space before depth-compare sampling.
    outShadowViewProj = lightViewProjVoxel * makePbrKmToVoxelMetersMatrix();
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
        useExternalShadowMap = renderSunShadowMap(params, projection, view, externalShadowViewProj);
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
    params.frustum.extractPlanes(viewProjection);

    const glm::vec3 lightDir = params.sky.getSunDir();
    const glm::vec3 lightColor = glm::vec3(1.0f, 0.98f, 0.96f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, params.chunkManager.atlas.atlasTextureArrayID);

    params.chunkShader.use();
    params.chunkShader.setMat4("viewProj", viewProjection);
    params.chunkShader.setInt("uOutputHdrLinear", useExternalSceneTextures ? 1 : 0);
    params.chunkShader.setFloat("uHdrTerrainExposureScale", useExternalSceneTextures ? 0.3f : 1.0f);
    // Keep physical depth units (km) to match the PbrSky sample integration path.
    params.chunkShader.setFloat("uAerialDepthScaleKm", 1.0f);
    const bool useChunkSunShadow = useExternalShadowMap && (m_SunShadowDepthTex != 0);
    params.chunkShader.setInt("uUseSunShadowMap", useChunkSunShadow ? 1 : 0);
    // Realistic path disables baked channel when dynamic sun shadows are active.
    // Fall back to baked only when dynamic map is unavailable to avoid totally flat lighting.
    const bool useBakedSunChannel = !params.sky.requiresExternalSceneTextures() || !useChunkSunShadow;
    params.chunkShader.setInt("uUseBakedSunChannel", useBakedSunChannel ? 1 : 0);
    if (useChunkSunShadow) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_SunShadowDepthTex);
        params.chunkShader.setInt("uSunShadowTex", 1);
        params.chunkShader.setMat4("uSunShadowViewProj", m_LastSunShadowViewProjVoxel);
        const float texelSize = 1.0f / static_cast<float>(std::max(1, m_SunShadowMapSize));
        params.chunkShader.setVec2("uSunShadowTexelSize", glm::vec2(texelSize));
    }
    else {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        params.chunkShader.setVec2("uSunShadowTexelSize", glm::vec2(0.0f));
    }

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
            params.chunkShader.setFloat("shadowContrast", 1.3f);
        }

        uniformsConfigured = true;
    }

    // Camera position changes every frame; keep this outside one-time static uniforms.
    params.chunkShader.setVec3("cameraPos", params.activeCamera.position);

    params.chunkShader.setInt("texture1", 0);
    glActiveTexture(GL_TEXTURE0);
    glPolygonMode(GL_FRONT_AND_BACK, params.toggleWireframe ? GL_LINE : GL_FILL);

    params.chunkManager.renderChunks(
        params.chunkShader,
        params.frustum,
        params.activeCamera.position,
        params.player.renderDistance
    );

    const glm::vec3 ambientColor = glm::vec3(0.36f, 0.40f, 0.46f);
    params.player.renderRemotePlayers(view, projection, lightDir, lightColor, ambientColor);

    // Debug passes
    if (params.toggleChunkBorders) {
        params.chunkManager.renderChunkBorders(view, projection);
    }

    if (params.toggleDebugFrustum) {
        params.frustum.drawFrustumFaces(
            params.debugShader,
            projection * view,
            view,
            projection,
            params.toggleWireframe
        );
    }

    if (params.renderOpaqueOverlayPasses) {
        params.renderOpaqueOverlayPasses();
    }

    if (useExternalSceneTextures) {
        // Build linear depth from the resolved scene depth buffer so every opaque pass
        // (chunks + remote players + world items) contributes consistently to AP.
        runDepthLinearizePass(projection);
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
            params.sky.setExternalShadowMap(m_SunShadowDepthTex, externalShadowViewProj);
        }
        else {
            params.sky.clearExternalShadowMap();
        }
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











