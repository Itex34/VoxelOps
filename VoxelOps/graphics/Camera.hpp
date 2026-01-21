#ifndef CAMERA_HPP
#define CAMERA_HPP

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera(glm::vec3 startPos);

    glm::mat4 getViewMatrix() const;
    void updateRotation(float newYaw, float newPitch);

    float getYaw() const;
    float getPitch() const;

    glm::vec3 direction;
    glm::vec3 position;
    glm::vec3 front;
    glm::vec3 XZfront;// for movement on the XZ plane
    glm::vec3 up;

private:
    float yaw;
    float pitch;
};

#endif // CAMERA_HPP
