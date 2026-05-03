#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RenderDeviceFactory.hpp"
#include "../../Shared/runtime/Paths.hpp"
#include <iostream>

using namespace AppHelpers;

void App::shutdown(Runtime &runtime) {
    runtime.network.clientNet.Shutdown();
    if (m_worldItemRenderer) {
        m_worldItemRenderer->shutdown();
        m_worldItemRenderer.reset();
    }
    if (runtime.ui.debugUi) {
        runtime.ui.debugUi->shutdown();
        runtime.ui.debugUi.reset();
    }
    runtime.ui.inventoryUi.reset();
    runtime.render.gunSceneRenderer.reset();
    runtime.render.gunRenderer.reset();
    if (runtime.render.renderer) {
        runtime.render.renderer->shutdown();
        runtime.render.renderer.reset();
    }

    if (m_GlContext != nullptr) {
        SDL_GL_DestroyContext(m_GlContext);
        m_GlContext = nullptr;
    }
    if (m_Window) {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }
    SDL_Quit();
}

int App::Run(int argc, char **argv) {
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

    m_RenderApi = ResolveRenderApiFromEnvironment();
    std::cout << "[App] Requested render API: " << GetRenderApiName(m_RenderApi) << "\n";

    if (!initWindowAndContext()) {
        return -1;
    }
    m_ShouldQuit = false;

    Runtime runtime;
    runtime.render.renderer = CreateRenderDevice(m_RenderApi);
    if (!runtime.render.renderer) {
        std::cerr << "[App] Failed to create render device.\n";
        return -1;
    }
    if (!runtime.render.renderer->initialize(m_Window)) {
        std::cerr << "[App] Failed to initialize render device.\n";
        return -1;
    }
    const RenderDeviceCapabilities caps = runtime.render.renderer->getCapabilities();
    std::cout << "[App] Render API selected: " << caps.apiName << "\n";
    m_worldItemRenderer = CreateWorldItemRenderer(caps.api);
    initGameplay(runtime);
    initCallbacks(runtime);
    initRenderResources(runtime);
    initUi(runtime);
    initNetworking(runtime);

    const double startTime = GetTimeSeconds();
    GameData::lastFrame = startTime;
    GameData::fpsTime = startTime;
    GameData::deltaTime = 0.0;

    while (!m_ShouldQuit) {
        processFrame(runtime);
    }

    shutdown(runtime);
    return 0;
}



