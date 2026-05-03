#include "OpenGLRenderDevice.hpp"
#include "../../ui/debug/DebugUi.hpp"

#include <glad/glad.h>
#include <SDL3/SDL.h>

RenderDeviceCapabilities OpenGLRenderDevice::getCapabilities() const noexcept {
    return RenderDeviceCapabilities{
        .api = RenderApi::OpenGL,
        .apiName = "OpenGL",
        .backendTier = m_renderer.getActiveBackend(),
        .backendName = m_renderer.getActiveBackendName(),
        .mdiUsable = m_renderer.isMDIUsable(),
        .supportsBakedChunkLighting = true,
        .supportsGiRuntimeControls = false,
        .supportsFirstPersonViewmodel = true,
        .compositesUiInRenderFrame = false
    };
}

bool OpenGLRenderDevice::initialize(SDL_Window *window) {
    (void)window;
    if (!m_renderer.initializeFrameResources()) {
        return false;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    return true;
}

void OpenGLRenderDevice::renderFrame(RenderScene &scene) {
    m_renderer.renderFrame(scene);
}

bool OpenGLRenderDevice::initializeDebugUi(
    DebugUi &debugUi, SDL_Window *window, void *nativeContext
) {
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
