#include "OpenGLRenderDevice.hpp"

int OpenGLRenderDevice::getOpenGLVersionMajor() const noexcept {
    return m_renderer.getBackend().getOpenGLVersionMajor();
}

int OpenGLRenderDevice::getOpenGLVersionMinor() const noexcept {
    return m_renderer.getBackend().getOpenGLVersionMinor();
}

GraphicsBackend OpenGLRenderDevice::getActiveBackend() const noexcept {
    return m_renderer.getActiveBackend();
}

std::string_view OpenGLRenderDevice::getActiveBackendName() const noexcept {
    return m_renderer.getActiveBackendName();
}

bool OpenGLRenderDevice::isMDIUsable() const noexcept {
    return m_renderer.isMDIUsable();
}

void OpenGLRenderDevice::renderFrame(RenderFrameParams &params) {
    m_renderer.renderFrame(params);
}

void OpenGLRenderDevice::shutdown() {
    m_renderer.shutdown();
}

std::string_view OpenGLRenderDevice::getApiName() const noexcept {
    return "OpenGL";
}
