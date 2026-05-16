#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RenderDeviceFactory.hpp"
#include "../../Shared/runtime/Paths.hpp"
#include <array>
#include <iostream>

using namespace AppHelpers;

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

    std::cout << "[App] Runtime paths: " << Shared::RuntimePaths::Describe() << "\n";
    std::cout << "[App] Network target: " << m_ServerIp << ":" << m_ServerPort;
    if (!m_RequestedUsername.empty()) {
        std::cout << " | requestedName=" << m_RequestedUsername;
    }
    std::cout << "\n";

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return -1;
    }

    const RenderApi requestedApi = ResolveRenderApiFromEnvironment();
    std::cout << "[App] Requested render API: " << GetRenderApiName(requestedApi) << "\n";

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
    initCallbacks(runtime);
    initRenderResources(runtime);
    initUi(runtime);
    initNetworking(runtime);
    rebindFrameOrchestrator(runtime);

    const double startTime = GetTimeSeconds();
    GameData::lastFrame = startTime;
    GameData::fpsTime = startTime;
    GameData::deltaTime = 0.0;

    while (!m_ShouldQuit) {
        m_frameOrchestrator.runFrame();
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



