#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "App.hpp"
#include "AppHelpers.hpp"

#include <iostream>

using namespace AppHelpers;

void App::shutdown(Runtime& runtime) {
    runtime.clientNet.Shutdown();
    m_worldItemRenderer.shutdown();
    if (runtime.debugUi) {
        runtime.debugUi->shutdown();
        runtime.debugUi.reset();
    }
    runtime.inventoryUi.reset();

    runtime.sky.shutdown();

    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    glfwTerminate();
}


int App::Run(int argc, char** argv) {
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

    if (!initWindowAndContext()) {
        return -1;
    }

    Runtime runtime;
    initGameplay(runtime);
    initCallbacks(runtime);
    initRenderResources(runtime);
    initUi(runtime);
    initNetworking(runtime);

    const double startTime = glfwGetTime();
    GameData::lastFrame = startTime;
    GameData::fpsTime = startTime;
    GameData::deltaTime = 0.0;

    while (!glfwWindowShouldClose(m_Window)) {
        processFrame(runtime);
    }

    shutdown(runtime);
    return 0;
}
