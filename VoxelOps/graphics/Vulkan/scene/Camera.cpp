#include "graphics/Vulkan/scene/Camera.hpp"
#include <cmath>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>

Camera::Camera(glm::vec3 startPos)
    : position(startPos), front(glm::vec3(0.0f, 0.0f, -1.0f)),
      XZfront(glm::vec3(0.0f, 0.0f, -1.0f)), up(glm::vec3(0.0f, 1.0f, 0.0f)), yaw(-90.0f),
      pitch(0.0f) {
    updateRotation(yaw, pitch);
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}

glm::mat4 Camera::getProjectionMatrix(float aspectRatio, float fovDegrees, float nearPlane,
                                      float farPlane) const {
    glm::mat4 projection =
        glm::perspectiveRH_ZO(glm::radians(fovDegrees), aspectRatio, nearPlane, farPlane);
    projection[1][1] *= -1.0f; // Vulkan clip space has inverted Y compared to OpenGL.
    return projection;
}

void Camera::updateRotation(float newYaw, float newPitch) {
    yaw = newYaw;
    pitch = std::clamp(newPitch, -89.0f, 89.0f);

    direction.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    direction.y = std::sin(glm::radians(pitch));
    direction.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));

    front = glm::normalize(direction);
    const glm::vec3 horizontalFront(front.x, 0.0f, front.z);
    if (glm::length(horizontalFront) > 0.0001f) {
        XZfront = glm::normalize(horizontalFront);
    } else {
        XZfront = glm::vec3(0.0f, 0.0f, -1.0f);
    }
}

float Camera::getYaw() const {
    return yaw;
}

float Camera::getPitch() const {
    return pitch;
}
