#include <glad/glad.h>
#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RenderDeviceFactory.hpp"
#include "../graphics/VulkanRenderDevice.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using namespace AppHelpers;

namespace {
RenderApi ParseRenderApiFromEnv() {
    const char *renderApiEnv = std::getenv("VOXELOPS_RENDER_API");
    if (renderApiEnv == nullptr) {
        return RenderApi::OpenGL;
    }

    std::string apiName = TrimAscii(std::string_view(renderApiEnv));
    for (char &c : apiName) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    if (apiName == "vulkan" || apiName == "vk") {
        return RenderApi::Vulkan;
    }
    return RenderApi::OpenGL;
}
} // namespace

void App::shutdown(Runtime &runtime) {
    runtime.clientNet.Shutdown();
    m_worldItemRenderer.shutdown();
    if (runtime.debugUi) {
        runtime.debugUi->shutdown();
        runtime.debugUi.reset();
    }
    runtime.inventoryUi.reset();

    if (runtime.sky) {
        runtime.sky->shutdown();
        runtime.sky.reset();
    }
    if (runtime.renderer) {
        runtime.renderer->shutdown();
        runtime.renderer.reset();
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

    m_RenderApi = ParseRenderApiFromEnv();
    std::cout << "[App] Requested render API: "
              << ((m_RenderApi == RenderApi::Vulkan) ? "Vulkan" : "OpenGL") << "\n";

    if (!initWindowAndContext()) {
        return -1;
    }
    m_ShouldQuit = false;

    Runtime runtime;
    runtime.renderer = CreateRenderDevice(m_RenderApi);
    if (!runtime.renderer) {
        std::cerr << "[App] Failed to create render device.\n";
        return -1;
    }
    if (m_RenderApi == RenderApi::Vulkan) {
        VulkanRenderDevice *vulkanDevice =
            dynamic_cast<VulkanRenderDevice *>(runtime.renderer.get());
        if (vulkanDevice == nullptr) {
            std::cerr << "[App] Render API selection mismatch: Vulkan requested but Vulkan device "
                         "cast failed.\n";
            return -1;
        }
        if (!vulkanDevice->initialize(m_Window)) {
            std::cerr << "[App] Failed to initialize Vulkan render device.\n";
            return -1;
        }
    }
    std::cout << "[App] Render API selected: " << runtime.renderer->getApiName() << "\n";
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
