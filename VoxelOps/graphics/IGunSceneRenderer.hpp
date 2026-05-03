#pragma once

#include <glm/vec3.hpp>

class Camera;
struct Runtime;

class IGunSceneRenderer {
public:
    virtual ~IGunSceneRenderer() = default;

    virtual void renderRemotePlayerGuns(
        Runtime &runtime, const Camera &activeCamera, const glm::vec3 &sunDirection
    ) = 0;
    virtual void
    renderHeldGun(Runtime &runtime, const Camera &activeCamera, const glm::vec3 &sunDirection) = 0;
};
