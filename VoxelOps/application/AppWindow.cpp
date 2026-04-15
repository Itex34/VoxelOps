#include <glad/glad.h>
#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <sstream>

using namespace AppHelpers;

namespace {
std::string toLowerCopy(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

bool tryParseBoolEnv(const char* name, bool& outValue) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return false;
    }

    const std::string value = toLowerCopy(TrimAscii(std::string_view(raw)));
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        outValue = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        outValue = false;
        return true;
    }
    return false;
}

bool isBenchmarkModeEnabled() {
    bool enabled = false;
    return tryParseBoolEnv("VOXELOPS_BENCHMARK", enabled) && enabled;
}

std::optional<int> getSwapIntervalOverrideFromEnv() {
    const char* raw = std::getenv("VOXELOPS_GL_SWAP_INTERVAL");
    if (raw == nullptr) {
        return std::nullopt;
    }

    try {
        return std::stoi(TrimAscii(std::string_view(raw)));
    }
    catch (...) {
        return std::nullopt;
    }
}
}

void App::updateFPSCounter() {
    GameData::frameCount++;
    const double currentTime = GetTimeSeconds();
    const double elapsedTime = currentTime - GameData::fpsTime;

    if (elapsedTime >= 2.0) {
        const double fps = GameData::frameCount / elapsedTime;
        std::stringstream ss;
        ss << "Voxel Ops - FPS: " << fps;
        SDL_SetWindowTitle(m_Window, ss.str().c_str());

        GameData::frameCount = 0;
        GameData::fpsTime = currentTime;
    }
}


void App::applyMouseInputModes() {
    if (!m_Window) {
        return;
    }

    const bool cursorEnabled = GameData::cursorEnabled;
    if (!SDL_SetWindowRelativeMouseMode(m_Window, !cursorEnabled)) {
        std::cerr << "Failed to set relative mouse mode: " << SDL_GetError() << "\n";
    }
    if (!cursorEnabled) {
        float discardX = 0.0f;
        float discardY = 0.0f;
        SDL_GetRelativeMouseState(&discardX, &discardY);
    }
}


void App::Exit() {
    m_ShouldQuit = true;
}


bool App::initWindowAndContext() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    if (m_RenderApi == RenderApi::Vulkan) {
        m_Window = SDL_CreateWindow(
            "Voxel Ops",
            GameData::screenWidth,
            GameData::screenHeight,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
        );
        if (!m_Window) {
            std::cerr << "SDL_CreateWindow (Vulkan) failed: " << SDL_GetError() << "\n";
            SDL_Quit();
            return false;
        }

        m_GlContext = nullptr;
        return true;
    }

    const auto createWindowForVersion = [&](int major, int minor) -> SDL_Window* {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        return SDL_CreateWindow(
            "Voxel Ops",
            GameData::screenWidth,
            GameData::screenHeight,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
        );
    };

    m_Window = createWindowForVersion(4, 3);
    if (!m_Window) {
        std::cerr << "OpenGL 4.3 context creation failed, retrying with OpenGL 3.3.\n";
        m_Window = createWindowForVersion(3, 3);
    }
    if (!m_Window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return false;
    }

    m_GlContext = SDL_GL_CreateContext(m_Window);
    if (!m_GlContext) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
        SDL_Quit();
        return false;
    }
    if (!SDL_GL_MakeCurrent(m_Window, m_GlContext)) {
        std::cerr << "SDL_GL_MakeCurrent failed: " << SDL_GetError() << "\n";
        SDL_GL_DestroyContext(m_GlContext);
        m_GlContext = nullptr;
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
        SDL_Quit();
        return false;
    }

    int swapInterval = 0;
    if (const std::optional<int> envSwapInterval = getSwapIntervalOverrideFromEnv()) {
        swapInterval = *envSwapInterval;
    }
    else if (isBenchmarkModeEnabled()) {
        swapInterval = 0;
    }

    if (!SDL_GL_SetSwapInterval(swapInterval)) {
        std::cerr << "SDL_GL_SetSwapInterval(" << swapInterval << ") failed: " << SDL_GetError() << "\n";
    }
    else {
        std::cout << "[App] OpenGL swap interval: " << swapInterval << "\n";
    }

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
        std::cerr << "Failed to initialize GLAD.\n";
        SDL_GL_DestroyContext(m_GlContext);
        m_GlContext = nullptr;
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
        SDL_Quit();
        return false;
    }

    printf("OpenGL version: %s\n", glGetString(GL_VERSION));
    printf("GLSL version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    return true;
}


void App::toggleFullscreen(SDL_Window* window)
{
    if (!window) {
        return;
    }

    m_IsFullscreen = !m_IsFullscreen;
    if (m_IsFullscreen) {
        SDL_GetWindowPosition(window, &GameData::windowedX, &GameData::windowedY);
        SDL_GetWindowSizeInPixels(window, &GameData::screenWidth, &GameData::screenHeight);
    }

    if (!SDL_SetWindowFullscreen(window, m_IsFullscreen)) {
        std::cerr << "SDL_SetWindowFullscreen failed: " << SDL_GetError() << "\n";
        m_IsFullscreen = !m_IsFullscreen;
        return;
    }

    if (!m_IsFullscreen) {
        SDL_SetWindowPosition(window, GameData::windowedX, GameData::windowedY);
        SDL_SetWindowSize(window, GameData::screenWidth, GameData::screenHeight);
    }
}
