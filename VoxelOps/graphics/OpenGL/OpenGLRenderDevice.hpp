#pragma once

#include "../IRenderDevice.hpp"
#include "Renderer.hpp"

class OpenGLRenderDevice final : public IRenderDevice {
public:
    OpenGLRenderDevice() = default;
    ~OpenGLRenderDevice() override = default;

    RenderDeviceCapabilities getCapabilities() const noexcept override;

    bool initialize(SDL_Window *window) override;
    void renderFrame(RenderScene &scene) override;
    bool initializeDebugUi(DebugUi &debugUi, SDL_Window *window, void *nativeContext) override;
    void present(SDL_Window *window) override;
    void shutdown() override;

private:
    Renderer m_renderer;
};
