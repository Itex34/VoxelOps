#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RenderDeviceFactory.hpp"
#include "../../Shared/runtime/Paths.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace AppHelpers;

namespace {
    bool IsRegularFile(const std::filesystem::path &path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }

    std::string JoinMissingPaths(const std::vector<std::filesystem::path> &paths) {
        std::ostringstream out;
        for (size_t i = 0; i < paths.size(); ++i) {
            out << "  - " << paths[i].generic_string();
            if (i + 1 < paths.size()) {
                out << "\n";
            }
        }
        return out.str();
    }

    std::filesystem::path ResolveShaderDir() {
#ifdef SHADER_DIR
        std::filesystem::path shaderDir = std::filesystem::path(SHADER_DIR);
        if (shaderDir.empty()) {
            return shaderDir;
        }
        std::error_code ec;
        if (shaderDir.is_relative()) {
            shaderDir = std::filesystem::absolute(shaderDir, ec);
        }
        if (!ec) {
            shaderDir = shaderDir.lexically_normal();
        }
        return shaderDir;
#else
        return {};
#endif
    }

    bool IsObviouslyMisconfiguredShaderDir(const std::filesystem::path &shaderDir) {
        if (shaderDir.empty()) {
            return true;
        }
        const std::string generic = shaderDir.generic_string();
        return generic == "/" || generic == "\\";
    }
} // namespace

bool App::initializeRenderBackendCore(Runtime &runtime, RenderApi api) {
    if (!initWindowAndContext(api)) {
        std::cerr << "[App] Failed to create window/context for " << GetRenderApiName(api) << ".\n";
        return false;
    }

    runtime.render.renderer = CreateRenderDevice(api);
    if (!runtime.render.renderer) {
        std::cerr << "[App] Failed to create render device for " << GetRenderApiName(api) << ".\n";
        shutdownRenderBackendCore(runtime);
        return false;
    }

    if (!runtime.render.renderer->initialize(m_Window)) {
        std::cerr << "[App] Failed to initialize render device for " << GetRenderApiName(api)
                  << ".\n";
        shutdownRenderBackendCore(runtime);
        return false;
    }

    const RenderDeviceCapabilities caps = runtime.render.renderer->getCapabilities();
    m_RenderApi = caps.api;
    m_RenderApiPreference = (caps.api == RenderApi::Vulkan) ? 1 : 0;
    std::cout << "[App] Render API selected: " << caps.apiName << "\n";
    runtime.render.worldItemRenderer = CreateWorldItemRenderer(caps.api);
    return true;
}

void App::shutdownRenderBackendCore(Runtime &runtime) {
    if (runtime.render.worldItemRenderer) {
        runtime.render.worldItemRenderer->shutdown();
        runtime.render.worldItemRenderer.reset();
    }
    if (runtime.ui.debugUi) {
        runtime.ui.debugUi->shutdown();
        runtime.ui.debugUi.reset();
    }
    if (runtime.ui.nativeUi) {
        runtime.ui.nativeUi->shutdown();
        runtime.ui.nativeUi.reset();
    }
    runtime.render.gunSceneRenderer.reset();
    if (runtime.render.gunRenderer) {
        runtime.render.gunRenderer->shutdown();
        runtime.render.gunRenderer.reset();
    }
    if (runtime.render.renderer) {
        runtime.render.renderer->shutdown();
        runtime.render.renderer.reset();
    }

    if (m_GlContext != nullptr) {
        SDL_GL_DestroyContext(m_GlContext);
        m_GlContext = nullptr;
    }
    if (m_Window != nullptr) {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }
}

bool App::switchRenderApi(Runtime &runtime, RenderApi targetApi) {
    if (targetApi == m_RenderApi) {
        return true;
    }

    const RenderApi previousApi = m_RenderApi;
    std::cout << "[App] Switching render API from " << GetRenderApiName(previousApi) << " to "
              << GetRenderApiName(targetApi) << ".\n";

    shutdownRenderBackendCore(runtime);
    if (!initializeRenderBackendCore(runtime, targetApi)) {
        std::cerr << "[App] Render API switch failed. Attempting to restore "
                  << GetRenderApiName(previousApi) << ".\n";
        if (!initializeRenderBackendCore(runtime, previousApi)) {
            std::cerr << "[App] Failed to restore previous render API.\n";
            return false;
        }
        configureBackendPolicy(runtime);
        if (runtime.gameplay.chunkManager) {
            runtime.gameplay.chunkManager->rebuildAllChunkMeshes(true);
        }
        initRenderResources(runtime);
        initUi(runtime);
        applyMouseInputModes();
        rebindFrameOrchestrator(runtime);
        return false;
    }

    configureBackendPolicy(runtime);
    if (runtime.gameplay.chunkManager) {
        runtime.gameplay.chunkManager->rebuildAllChunkMeshes(true);
    }
    initRenderResources(runtime);
    initUi(runtime);
    applyMouseInputModes();
    rebindFrameOrchestrator(runtime);
    m_RenderApiPreference = (m_RenderApi == RenderApi::Vulkan) ? 1 : 0;
    return true;
}

void App::shutdown(Runtime &runtime) {
    runtime.network.clientNet.Shutdown();
    shutdownRenderBackendCore(runtime);
    SDL_Quit();
}

int App::run(int argc, char **argv) {
    LaunchOptions options;
    if (!ParseLaunchOptions(argc, argv, options)) {
        PrintUsage();
        return 2;
    }
    if (options.showHelp) {
        PrintUsage();
        return 0;
    }

    m_ServerIp = options.serverIp;
    m_ServerPort = options.serverPort;
    m_RequestedUsername = options.requestedUsername;
    m_BotMode = options.botMode;
    m_BotDurationSeconds = options.botDurationSeconds;
    m_BotSeed = options.botSeed;
    m_BotShootRate = options.botShootRate;
    m_BotRenderDistance = options.botRenderDistance;
    m_BotMinimizeWindow = options.botMinimizeWindow;

    std::cout << "[App] Runtime paths: " << Shared::RuntimePaths::Describe() << "\n";
    std::cout << "[App] Network target: " << m_ServerIp << ":" << m_ServerPort;
    if (!m_RequestedUsername.empty()) {
        std::cout << " | requestedName=" << m_RequestedUsername;
    }
    if (m_BotMode) {
        std::cout << " | bot=on"
                  << " durationSec=" << m_BotDurationSeconds << " seed=" << m_BotSeed
                  << " shootRate=" << m_BotShootRate << " renderDistance=" << m_BotRenderDistance;
    }
    std::cout << "\n";

    {
        const std::array<std::filesystem::path, 1> requiredFiles = {
            Shared::RuntimePaths::ResolveVoxelOpsPath("Assets/fonts/SF/SF-Pro-Text-Medium.otf")
        };

        std::vector<std::filesystem::path> missingFiles;
        for (const auto &path : requiredFiles) {
            if (!IsRegularFile(path)) {
                missingFiles.push_back(path);
            }
        }
        if (!missingFiles.empty()) {
            std::cerr << "[App][fatal] Missing required runtime files:\n"
                      << JoinMissingPaths(missingFiles) << "\n";
            return -1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return -1;
    }

    RenderApi requestedApi = ResolveRenderApiFromEnvironment();
    std::cout << "[App] Requested render API: " << GetRenderApiName(requestedApi) << "\n";
    if (requestedApi == RenderApi::Vulkan) {
        std::vector<std::filesystem::path> missingShaders;
        const std::filesystem::path shaderDir = ResolveShaderDir();
        if (IsObviouslyMisconfiguredShaderDir(shaderDir)) {
            std::cerr << "[App] Vulkan preflight failed: SHADER_DIR is invalid ('"
                      << shaderDir.generic_string() << "'). Reconfigure CMake and rebuild.\n";
            std::cout << "[App] Falling back to OpenGL.\n";
            requestedApi = RenderApi::OpenGL;
        } else {
            const std::array<const char *, 8> requiredShaders = {
                "VoxelTerrainGI.vert.spv",
                "VoxelTerrainGI.frag.spv",
                "VoxelTerrainGI_rt.frag.spv",
                "model.vert.spv",
                "model.frag.spv",
                "model_rt.frag.spv",
                "postprocess.vert.spv",
                "postprocess.frag.spv"
            };
            for (const char *fileName : requiredShaders) {
                const std::filesystem::path shaderPath = shaderDir / fileName;
                if (!IsRegularFile(shaderPath)) {
                    missingShaders.push_back(shaderPath);
                }
            }

            if (!missingShaders.empty()) {
#if defined(VOXELOPS_VULKAN_SHADERS_BUILT) && (VOXELOPS_VULKAN_SHADERS_BUILT == 0)
                std::cerr << "[App] Vulkan preflight failed: shaders were not built "
                             "(VOXELOPS_OPENGL_ONLY=ON at configure time).\n";
#else
                std::cerr
                    << "[App] Vulkan preflight failed: missing compiled SPIR-V shader files.\n";
#endif
                std::cerr << "[App] Expected shader directory: " << shaderDir.generic_string()
                          << "\n";
                std::cerr << JoinMissingPaths(missingShaders) << "\n";
                std::cout << "[App] Falling back to OpenGL.\n";
                requestedApi = RenderApi::OpenGL;
            }
        }
    }

    Runtime runtime;
    const std::array<RenderApi, 2> startupCandidates{requestedApi, RenderApi::OpenGL};
    const size_t startupCandidateCount = (requestedApi == RenderApi::Vulkan) ? 2u : 1u;
    bool renderBackendInitialized = false;
    for (size_t i = 0; i < startupCandidateCount; ++i) {
        const RenderApi candidate = startupCandidates[i];
        if (initializeRenderBackendCore(runtime, candidate)) {
            renderBackendInitialized = true;
            break;
        }
        shutdownRenderBackendCore(runtime);
        if (i + 1u < startupCandidateCount) {
            std::cout << "[App] Falling back to " << GetRenderApiName(startupCandidates[i + 1u])
                      << ".\n";
        }
    }

    if (!renderBackendInitialized) {
        SDL_Quit();
        return -1;
    }

    m_ShouldQuit = false;
    initGameplay(runtime);
    if (m_BotMode && runtime.gameplay.player) {
        runtime.gameplay.player->renderDistance = m_BotRenderDistance;
    }
    initCallbacks(runtime);
    initRenderResources(runtime);
    initUi(runtime);
    initNetworking(runtime);
    rebindFrameOrchestrator(runtime);
    if (m_BotMode) {
        //runtime.ui.wantsCursor = false;
        GameData::cursorEnabled = false;
        if (m_BotMinimizeWindow && m_Window != nullptr) {
            SDL_MinimizeWindow(m_Window);
        }
        if (!beginConnectionAttempt(runtime)) {
            std::cerr << "[bot] initial connection attempt failed; auto-reconnect may retry\n";
        }
    }

    const double startTime = GetTimeSeconds();
    m_BotStartTime = startTime;
    GameData::lastFrame = startTime;
    GameData::fpsTime = startTime;
    GameData::deltaTime = 0.0;

    while (!m_ShouldQuit) {
        m_frameOrchestrator.runFrame();
        if (m_BotMode && m_BotDurationSeconds > 0.0 &&
            (GetTimeSeconds() - m_BotStartTime) >= m_BotDurationSeconds) {
            std::cout << "[bot] duration elapsed, shutting down\n";
            m_ShouldQuit = true;
        }
        const bool requestedOpenGl = m_RequestSwitchToOpenGL;
        const bool requestedVulkan = m_RequestSwitchToVulkan;
        if (requestedOpenGl || requestedVulkan) {
            m_RequestSwitchToOpenGL = false;
            m_RequestSwitchToVulkan = false;

            const RenderApi targetApi = requestedVulkan ? RenderApi::Vulkan : RenderApi::OpenGL;
            if (!switchRenderApi(runtime, targetApi)) {
                std::cerr << "[App] Renderer switch failed; continuing with active backend.\n";
            }
            if (!runtime.render.renderer || m_Window == nullptr) {
                std::cerr << "[App] No active renderer after switch handling; shutting down.\n";
                m_ShouldQuit = true;
            }
        }
    }

    shutdown(runtime);
    return 0;
}
