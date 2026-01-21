#include "Camera.hpp"

Camera::Camera(glm::vec3 startPos)
    : position(startPos), front(glm::vec3(0.0f, 0.0f, -1.0f)),
    XZfront(glm::vec3(0.0f, 0.0f, -1.0f)),
    up(glm::vec3(0.0f, 1.0f, 0.0f)), yaw(-90.0f), pitch(0.0f) {
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}

void Camera::updateRotation(float newYaw, float newPitch) {
    yaw = newYaw;
    pitch = newPitch;



    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));

    front = glm::normalize(direction);
    XZfront = glm::normalize(glm::vec3(front.x, 0.0f, front.z));
}

float Camera::getYaw() const {
    return yaw;
}

float Camera::getPitch() const {
    return pitch;
}

