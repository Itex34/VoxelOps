#pragma once

#include "IRenderDevice.hpp"
#include "Renderer.hpp"

class OpenGLRenderDevice final : public IRenderDevice {
public:
    OpenGLRenderDevice() = default;
    ~OpenGLRenderDevice() override = default;

    int getOpenGLVersionMajor() const noexcept override;
    int getOpenGLVersionMinor() const noexcept override;
    GraphicsBackend getActiveBackend() const noexcept override;
    std::string_view getActiveBackendName() const noexcept override;
    bool isMDIUsable() const noexcept override;

    void renderFrame(RenderFrameParams& params) override;
    void shutdown() override;

    std::string_view getApiName() const noexcept override;

private:
    Renderer m_renderer;
};
