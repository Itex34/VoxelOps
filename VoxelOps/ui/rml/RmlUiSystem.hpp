#pragma once

#include "../../graphics/RenderApi.hpp"

#include <memory>

union SDL_Event;
struct SDL_Window;
namespace Rml {
    class Context;
}

class RmlUiSystem final {
public:
    RmlUiSystem();
    ~RmlUiSystem();

    bool initialize(SDL_Window *window, RenderApi api);
    void shutdown();
    void onWindowResized(int width, int height);
    void processEvent(const SDL_Event &event);
    void update();
    void render();

    bool isInitialized() const noexcept;
    bool wantsMouseCapture() const noexcept;
    bool wantsKeyboardCapture() const noexcept;
    bool isUsingOpenGlBackend() const noexcept;
    Rml::Context *context() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
