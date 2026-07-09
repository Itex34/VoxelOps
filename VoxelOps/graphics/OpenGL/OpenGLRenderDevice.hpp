#pragma once

#include "../IRenderDevice.hpp"
#include "Renderer.hpp"
#include "OpenGlNativeUiRenderer.hpp"

class OpenGLRenderDevice final : public IRenderDevice {
public:
    OpenGLRenderDevice() = default;
    ~OpenGLRenderDevice() override = default;

    RenderDeviceCapabilities getCapabilities() const noexcept override;

    bool initialize(SDL_Window *window) override;
    void renderFrame(RenderScene &scene) override;
    void onWindowResized(int width, int height) override;
    bool initializeDebugUi(DebugUi &debugUi, SDL_Window *window, void *nativeContext) override;
    void present(SDL_Window *window) override;
    void shutdown() override;

private:
    Renderer m_renderer;
    OpenGlNativeUiRenderer m_nativeUiRenderer;
};
