#pragma once

#include "../IGunSceneRenderer.hpp"

class VulkanGunSceneRenderer final : public IGunSceneRenderer {
  public:
    void renderRemotePlayerGuns(Runtime &runtime, const Camera &activeCamera,
                                const glm::vec3 &sunDirection) override {
        (void)runtime;
        (void)activeCamera;
        (void)sunDirection;
    }

    void renderHeldGun(Runtime &runtime, const Camera &activeCamera,
                       const glm::vec3 &sunDirection) override {
        (void)runtime;
        (void)activeCamera;
        (void)sunDirection;
    }
};
