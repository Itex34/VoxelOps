#include "OpenGLRenderDevice.hpp"
#include "../../ui/debug/DebugUi.hpp"

#include <SDL3/SDL.h>

RenderDeviceCapabilities OpenGLRenderDevice::getCapabilities() const noexcept {
    const Backend &backend = m_renderer.getBackend();
    const int glMajor = backend.getOpenGLVersionMajor();
    const int glMinor = backend.getOpenGLVersionMinor();
    return RenderDeviceCapabilities{
        .api = RenderApi::OpenGL,
        .apiName = "OpenGL",
        .backendTier = m_renderer.getActiveBackend(),
        .backendName = m_renderer.getActiveBackendName(),
        .mdiUsable = m_renderer.isMDIUsable(),
        .supportsGL43Shaders = (glMajor > 4) || (glMajor == 4 && glMinor >= 3),
        .supportsBakedChunkLighting = true,
        .supportsGiRuntimeControls = false,
        .supportsFirstPersonViewmodel = true,
        .compositesUiInRenderFrame = false,
        .requiresOpenGlStateSetup = true};
}

bool OpenGLRenderDevice::initialize(SDL_Window *window) {
    (void)window;
    return m_renderer.initializeFrameResources();
}

void OpenGLRenderDevice::renderFrame(RenderFrameParams &params) {
    m_renderer.renderFrame(params);
}

bool OpenGLRenderDevice::initializeDebugUi(DebugUi &debugUi, SDL_Window *window,
                                           void *nativeContext) {
    SDL_GLContext glContext = reinterpret_cast<SDL_GLContext>(nativeContext);
    return debugUi.initialize(window, glContext, "#version 330");
}

void OpenGLRenderDevice::present(SDL_Window *window) {
    if (window != nullptr) {
        SDL_GL_SwapWindow(window);
    }
}

void OpenGLRenderDevice::shutdown() {
    m_renderer.shutdown();
}
