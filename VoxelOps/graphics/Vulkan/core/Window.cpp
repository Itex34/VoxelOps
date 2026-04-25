#include "graphics/Vulkan/core/Window.hpp"

void Window::init() {
    if (handle) {
        cleanup();
    }

    handle = SDL_CreateWindow("Vulkan SDL3", 800, 600, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!handle) {
        throw std::runtime_error("Failed to create window");
    }
}

void Window::cleanup() {
    if (handle) {
        SDL_DestroyWindow(handle);
        handle = nullptr;
    }
}

Window::~Window() {
    cleanup();
}

void Window::pollEvents(bool &running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            int pixelW = 0;
            int pixelH = 0;
            SDL_GetWindowSizeInPixels(handle, &pixelW, &pixelH);
            framebufferResized = true;
            minimized = (pixelW == 0 || pixelH == 0);
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED:
            minimized = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            minimized = false;
            framebufferResized = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == SDL_GetWindowID(handle)) {
                mouseCaptureRequested = true;
            }
            break;
        default:
            break;
        }
    }
}

bool Window::consumeResizeFlag() {
    const bool resized = framebufferResized;
    framebufferResized = false;
    return resized;
}

bool Window::consumeMouseCaptureRequest() {
    const bool requested = mouseCaptureRequested;
    mouseCaptureRequested = false;
    return requested;
}
