#pragma once

#include <glm/mat4x4.hpp>

class Camera;
class ISkyBackend;

class OpenGLSkyPass {
  public:
    void renderDirect(ISkyBackend &sky, const Camera &activeCamera, const glm::mat4 &projection,
                      const glm::mat4 &view) const;
    void compositeExternal(ISkyBackend &sky, const Camera &activeCamera, const glm::mat4 &projection,
                           const glm::mat4 &view) const;
};
