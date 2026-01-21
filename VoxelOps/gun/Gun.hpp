#pragma once

#include "../graphics/Model.hpp"
#include "../physics/RayManager.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

class Shader; // forward

class Gun {
public:
    // seconds between shots, reload time in seconds
    Gun(float inFireInterval = 0.2f, float inReloadTime = 3.0f) noexcept;
    ~Gun() = default;

    // Non-copyable (models usually not copyable). Movable allowed.
    Gun(const Gun&) = delete;
    Gun& operator=(const Gun&) = delete;
    Gun(Gun&&) noexcept = default;
    Gun& operator=(Gun&&) noexcept = default;

    // Input: call when player presses fire
    void requestFire() noexcept;

    // Update internal timers and perform fire when allowed.
    // rayOrigin / rayDirection are used when firing.
    // deltaTime is seconds elapsed since last update.
    void update(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float deltaTime);

    // immediate fire (performs raycast / effects). Usually internal.
    void fire(const glm::vec3& rayOrigin, const glm::vec3& rayDirection);

    // Render the gun model relative to given transform
    void render(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale, Shader& shader) const;

    // Begin reload (if not already). Non-blocking: update handles the timer.
    void reload() noexcept;

    // Load gun model. Returns true on success.
    bool loadModel(const std::string& path);

    // getters
    unsigned int getCurrentAmmo() const noexcept { return currentAmmo; }
    unsigned int getMaxAmmo() const noexcept { return maxAmmo; }
    bool isReloadingNow() const noexcept { return isReloading; }

public:
    glm::vec3 gunCamOffset = glm::vec3(0.08f, -0.05f, -0.12f); // example typical values (meters)
    glm::vec3 hitPoint = glm::vec3(0.0f);

    static constexpr float maxShootDistance = 10000.0f;

private:
    std::unique_ptr<Model> gunModel;

    // settings
    float reloadTime;     // seconds
    float fireInterval;   // seconds per shot

    // state
    float timeSinceLastShot = 0.0f;
    bool wantsToFire = false;

    bool isReloading = false;
    float reloadTimer = 0.0f;

    unsigned int maxAmmo = 30;
    unsigned int currentAmmo = maxAmmo;

    // internal helper
    void tryFireIfReady(const glm::vec3& rayOrigin, const glm::vec3& rayDirection);
};
