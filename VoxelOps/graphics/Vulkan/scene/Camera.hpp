#pragma once

#include <glm/glm.hpp>

class Camera {
public:
    explicit Camera(glm::vec3 startPos);

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspectRatio, float fovDegrees = 70.0f, float nearPlane = 0.1f, float farPlane = 1000.0f) const;
    void updateRotation(float newYaw, float newPitch);

    float getYaw() const;
    float getPitch() const;

    glm::vec3 direction;
    glm::vec3 position;// viewer's eye
    glm::vec3 front;
    glm::vec3 XZfront;// for movement on the XZ plane
    glm::vec3 up;

private:
    float yaw;
    float pitch;
};
