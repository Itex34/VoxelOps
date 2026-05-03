#pragma once

#include <SDL3/SDL.h>

#include <stdexcept>

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    Window(Window &&) = delete;
    Window &operator=(Window &&) = delete;

    void init();
    void cleanup();
    void pollEvents(bool &running);
    bool consumeResizeFlag();
    bool consumeMouseCaptureRequest();
    bool isMinimized() const {
        return minimized;
    }
    SDL_Window *getHandle() const {
        return handle;
    }

    uint32_t getWidth() const {
        if (!handle) {
            throw std::runtime_error("Window not initialized");
        }
        int w;
        SDL_GetWindowSizeInPixels(handle, &w, nullptr);
        return static_cast<uint32_t>(w);
    }

    uint32_t getHeight() const {
        if (!handle) {
            throw std::runtime_error("Window not initialized");
        }
        int h;
        SDL_GetWindowSizeInPixels(handle, nullptr, &h);
        return static_cast<uint32_t>(h);
    }

private:
    SDL_Window *handle = nullptr;
    bool framebufferResized = false;
    bool minimized = false;
    bool mouseCaptureRequested = false;
};
