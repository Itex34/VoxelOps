#pragma once

#include "../IGunSceneRenderer.hpp"

class OpenGLGunSceneRenderer final : public IGunSceneRenderer {
public:
    void renderRemotePlayerGuns(
        Runtime &runtime, const Camera &activeCamera, const glm::vec3 &sunDirection
    ) override;
    void renderHeldGun(
        Runtime &runtime, const Camera &activeCamera, const glm::vec3 &sunDirection
    ) override;
};
