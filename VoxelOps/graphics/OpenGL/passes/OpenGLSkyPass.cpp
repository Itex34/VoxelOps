#include "OpenGLSkyPass.hpp"

#include "../../ISkyBackend.hpp"
#include "../../Camera.hpp"

#include <glad/glad.h>

void OpenGLSkyPass::renderDirect(ISkyBackend &sky, const Camera &activeCamera,
                                 const glm::mat4 &projection, const glm::mat4 &view) const {
    sky.setCameraFromActiveCamera(activeCamera);
    const bool framebufferSrgbWasEnabled = (glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE);
    if (sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
    sky.render(projection, view);
    if (sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
}

void OpenGLSkyPass::compositeExternal(ISkyBackend &sky, const Camera &activeCamera,
                                      const glm::mat4 &projection, const glm::mat4 &view) const {
    sky.setCameraFromActiveCamera(activeCamera);
    const bool framebufferSrgbWasEnabled = (glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE);
    if (sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
    sky.render(projection, view);
    if (sky.encodesOutputToSrgb() && framebufferSrgbWasEnabled) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
}
