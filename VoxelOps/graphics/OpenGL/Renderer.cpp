

#include "Renderer.hpp"
#include "ShaderSkyBackend.hpp"
#include "Shader.hpp"
#include "../Camera.hpp"
#include "../ISkyBackend.hpp"
#include "../../data/GameData.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/common.hpp>
#include <iostream>
#include <string>

namespace {
    void applyOpaqueWorldState() {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glDisable(GL_BLEND);
    }

    void drainPendingRendererGlErrors(const char *stageTag) {
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
                std::cerr << "[Renderer] Cleared " << count << " pending GL error(s) after "
                          << stageTag << " (first=0x" << std::hex << firstError << std::dec
                          << ").\n";
                ++loggedCount;
            }
        }
    }
} // namespace

Renderer::~Renderer() = default;

const Backend &Renderer::getBackend() const noexcept {
    return m_ActiveBackend;
}

GraphicsBackend Renderer::getActiveBackend() const noexcept {
    return m_ActiveBackend.getActiveBackend();
}

std::string_view Renderer::getActiveBackendName() const noexcept {
    return m_ActiveBackend.getActiveBackendName();
}

bool Renderer::isMDIUsable() const noexcept {
    return m_ActiveBackend.isMDIUsable();
}

bool Renderer::initializeFrameResources() {
    return ensureChunkAndSkyResources();
}

bool Renderer::ensureChunkAndSkyResources() {
    if (m_ChunkShader && m_DebugShader && m_SkyBackend && m_textureAtlas.getArrayTextureId() != 0) {
        return true;
    }

    const int glMajor = m_ActiveBackend.getOpenGLVersionMajor();
    const int glMinor = m_ActiveBackend.getOpenGLVersionMinor();
    const bool supportsGL43Shaders = (glMajor > 4) || (glMajor == 4 && glMinor >= 3);

    const std::string chunkVertPath =
        supportsGL43Shaders
            ? Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack.vert")
                  .generic_string()
            : Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack33.vert")
                  .generic_string();
    const std::string chunkFragPath =
        supportsGL43Shaders
            ? Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack.frag")
                  .generic_string()
            : Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack33.frag")
                  .generic_string();
    const std::string skyVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/sky.vert").generic_string();
    const std::string skyFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/sky_simple.frag").generic_string();
    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();

    m_ChunkShader = std::make_unique<Shader>(chunkVertPath.c_str(), chunkFragPath.c_str());
    m_DebugShader = std::make_unique<Shader>(debugVertPath.c_str(), debugFragPath.c_str());
    m_SkyBackend = std::make_unique<ShaderSkyBackend>(skyVertPath, skyFragPath);
    m_SkyBackend->initialize();
    if (!m_textureAtlas.initialize()) {
        std::cerr << "[Renderer] Failed to initialize OpenGL texture atlas array.\n";
        return false;
    }

    static constexpr glm::vec3 kDefaultSunDirection = glm::vec3(0.0f, 0.43496552f, 0.90044713f);
    m_SkyBackend->setSunDir(kDefaultSunDirection);
    m_SkyBackend->resize(GameData::screenWidth, GameData::screenHeight);
    return true;
}

void Renderer::shutdown() {
    m_remotePlayerRenderer.shutdown();
    if (m_SkyBackend) {
        m_SkyBackend->shutdown();
        m_SkyBackend.reset();
    }
    m_ChunkShader.reset();
    m_DebugShader.reset();
    m_textureAtlas.cleanup();
    m_ChunkUniformsInitialized = false;
}

void Renderer::beginFrame() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::endFrame() {
    // swap buffers (outside if handled elsewhere)
}

void Renderer::renderFrame(RenderScene &params) {
    if (!ensureChunkAndSkyResources() || m_ChunkShader == nullptr || m_DebugShader == nullptr ||
        m_SkyBackend == nullptr) {
        static bool loggedMissingOwnedResources = false;
        if (!loggedMissingOwnedResources) {
            std::cerr << "[Renderer] Skipping frame: failed to initialize OpenGL chunk/sky "
                         "resources.\n";
            loggedMissingOwnedResources = true;
        }
        return;
    }

    Shader &chunkShader = *m_ChunkShader;
    Shader &debugShader = *m_DebugShader;
    ISkyBackend &sky = *m_SkyBackend;
    if (params.chunkWorld.cpuChunkMeshes == nullptr) {
        return;
    }
    m_chunkScene.syncFromCpuChunkMeshes(*params.chunkWorld.cpuChunkMeshes);

    drainPendingRendererGlErrors("frame start");
    const Camera &cullingCamera =
        params.cullingCamera ? *params.cullingCamera : params.activeCamera;

    int frameWidth = GameData::screenWidth;
    int frameHeight = GameData::screenHeight;
    if (frameWidth <= 0 || frameHeight <= 0) {
        GLint viewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, viewport);
        frameWidth = viewport[2];
        frameHeight = viewport[3];
    }
    if (frameWidth <= 0 || frameHeight <= 0) {
        static bool loggedInvalidFrameSize = false;
        if (!loggedInvalidFrameSize) {
            std::cerr << "[Renderer] Skipping frame: invalid framebuffer size " << frameWidth << "x"
                      << frameHeight << ".\n";
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
    sky.resize(frameWidth, frameHeight);
    sky.setSunDir(params.sunDirection);
    sky.setExposure(params.skyExposure);
    sky.setViewFovYDegrees(GameData::FOV);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, frameWidth, frameHeight);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    beginFrame();
    glEnable(GL_MULTISAMPLE);
    m_skyPass.renderDirect(sky, params.activeCamera, projection, view);

    // Backends may alter global GL state during sky/fullscreen passes.
    // Re-apply world-pass state to keep chunk rendering deterministic.
    applyOpaqueWorldState();

    // World pass
    const glm::mat4 viewProjection = projection * view;
    const glm::mat4 cullingViewProjection = projection * cullingCamera.getViewMatrix();

    const glm::vec3 lightDir = params.sunDirection;
    const glm::vec3 lightColor = glm::vec3(1.0f, 0.98f, 0.96f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_textureAtlas.getArrayTextureId());
    drainPendingRendererGlErrors("chunk pass bind atlas");

    chunkShader.use();
    drainPendingRendererGlErrors("chunk pass use shader");
    chunkShader.setMat4("viewProj", viewProjection);
    chunkShader.setInt("uOutputHdrLinear", 0);
    // Keep terrain slightly darker than sky for better separation.
    chunkShader.setFloat("uHdrTerrainExposureScale", 0.92f);
    // Performance path uses direct sky and baked AO only.
    chunkShader.setFloat("uAerialDepthScaleKm", 1.0f);
    chunkShader.setInt("uUseSunShadowMap", 0);
    // Keep sampler bindings deterministic to avoid driver draw-time sampler-type conflicts.
    chunkShader.setInt("texture1", 0);
    chunkShader.setInt("uSunShadowTexNear", 1);
    chunkShader.setInt("uSunShadowTexFar", 2);
    chunkShader.setInt("uSunShadowMomentsNear", 3);
    chunkShader.setFloat(
        "uSunShadowLowSunBiasBoost", glm::clamp(params.sunShadowLowSunBiasBoost, 0.0f, 4.0f)
    );
    chunkShader.setInt("uUseBakedSunChannel", 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    chunkShader.setInt("uUseSunShadowMomentsNear", 0);
    chunkShader.setVec2("uSunShadowTexelSizeNear", glm::vec2(0.0f));
    chunkShader.setVec2("uSunShadowTexelSizeFar", glm::vec2(0.0f));
    chunkShader.setFloat("uSunShadowSplitDepthKm", 0.0f);
    chunkShader.setFloat("uSunShadowBlendKm", 0.001f);
    chunkShader.setVec3("uSunShadowDirectionalBias", params.sunShadowDirectionalBias);
    drainPendingRendererGlErrors("chunk pass uniforms setup");

    bool &uniformsConfigured = m_ChunkUniformsInitialized;
    chunkShader.setVec3("lightDir", lightDir);
    chunkShader.setVec3("lightColor", lightColor);
    if (!uniformsConfigured) {
        if (params.chunkWorld.enableAO) {
            chunkShader.setVec3("skyColorTop", glm::vec3(0.58f, 0.73f, 0.95f));
            chunkShader.setVec3("skyColorBottom", glm::vec3(0.86f, 0.91f, 0.98f));

            chunkShader.setFloat("ambientStrength", 0.89f);
            chunkShader.setFloat("diffuseStrength", 0.85f);
            chunkShader.setFloat("minAmbient", 0.01f);

            chunkShader.setFloat("hemiTint", 0.5f);
            chunkShader.setFloat("contrast", 1.0f);
            chunkShader.setFloat("satBoost", 1.17f);
            chunkShader.setVec3("warmth", glm::vec3(1.03f, 1.00f, 0.97f));

            chunkShader.setFloat("aoPow", 0.8f);
            chunkShader.setFloat("aoMin", 0.6f);
            chunkShader.setFloat("aoApplyAfterTone", 0.8f);

            chunkShader.setFloat("shadowDarkness", 0.3f);
            chunkShader.setFloat("shadowContrast", 1.15f);
        }

        uniformsConfigured = true;
    }

    // Camera position changes every frame; keep this outside one-time static uniforms.
    chunkShader.setVec3("cameraPos", params.activeCamera.position);
    glActiveTexture(GL_TEXTURE0);
    OpenGLWorldPassInput worldPassInput{
        .chunkShader = chunkShader,
        .debugShader = debugShader,
        .chunkScene = m_chunkScene,
        .remotePlayerRenderer = m_remotePlayerRenderer,
        .frameParams = params,
        .view = view,
        .projection = projection,
        .cullingViewProjection = cullingViewProjection,
        .lightDir = lightDir,
        .lightColor = lightColor,
        .toggleWireframe = params.toggleWireframe,
        .toggleChunkBorders = params.toggleChunkBorders,
        .toggleDebugFrustum = params.toggleDebugFrustum
    };
    m_worldPass.execute(worldPassInput);

    // Keep downstream passes (held gun, UI overlays) deterministic.
    applyOpaqueWorldState();
}

