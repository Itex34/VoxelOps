#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RenderDeviceFactory.hpp"
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <sstream>

using namespace AppHelpers;

namespace {
    std::string toLowerCopy(std::string value) {
        for (char &c : value) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return value;
    }

    bool tryParseBoolEnv(const char *name, bool &outValue) {
        const char *raw = std::getenv(name);
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
        const char *raw = std::getenv("VOXELOPS_GL_SWAP_INTERVAL");
        if (raw == nullptr) {
            return std::nullopt;
        }

        try {
            return std::stoi(TrimAscii(std::string_view(raw)));
        } catch (...) {
            return std::nullopt;
        }
    }
} // namespace

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

bool App::initWindowAndContext(RenderApi api) {
    int swapInterval = 0;
    if (const std::optional<int> envSwapInterval = getSwapIntervalOverrideFromEnv()) {
        swapInterval = *envSwapInterval;
    } else if (isBenchmarkModeEnabled()) {
        swapInterval = 0;
    }

    RenderBackendWindowContext backendWindow{};
    if (!CreateRenderBackendWindowContext(
            api, "Voxel Ops", GameData::screenWidth, GameData::screenHeight, swapInterval,
            backendWindow
        )) {
        return false;
    }

    m_Window = backendWindow.window;
    m_GlContext = backendWindow.glContext;
    return true;
}

void App::toggleFullscreen(SDL_Window *window) {
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
