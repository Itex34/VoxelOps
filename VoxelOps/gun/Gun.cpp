#include "Gun.hpp"

#include <iostream>
#include <stdexcept>

// Constructor
Gun::Gun(float inFireInterval, float inReloadTime) noexcept :
    reloadTime(inReloadTime),
    fireInterval(inFireInterval),
    timeSinceLastShot(0.0f),
    wantsToFire(false),
    isReloading(false),
    reloadTimer(0.0f),
    maxAmmo(30),
    currentAmmo(30)
{
}

// Public API
void Gun::requestFire() noexcept {
    wantsToFire = true;
}

void Gun::update(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float deltaTime) {
    // Advance timers
    timeSinceLastShot += deltaTime;

    if (isReloading) {
        reloadTimer += deltaTime;
        if (reloadTimer >= reloadTime) {
            currentAmmo = maxAmmo;
            isReloading = false;
            reloadTimer = 0.0f;
            std::cout << "Reload complete.\n";
        }
        // While reloading we don't shoot.
        return;
    }

    // If out of ammo and not already reloading, initiate reload.
    if (currentAmmo == 0 && !isReloading) {
        reload();
        return;
    }

    // Attempt to fire if requested and ready.
    tryFireIfReady(rayOrigin, rayDirection);

    // For semi-auto use: clear the intent so a single request -> single shot.
    // If you want auto-fire (hold to keep shooting) remove the next line.
    wantsToFire = false;

    // Debug info
    // std::cout << "Current ammo: " << currentAmmo << "\n";
}

void Gun::fire(const glm::vec3& rayOrigin, const glm::vec3& rayDirection) {
    glm::vec3 normDirection = glm::normalize(rayDirection);

    // Default behavior: assume no hit and set hitPoint to max distance.
    hitPoint = rayOrigin + normDirection * maxShootDistance;

    // =========================
    // RAYCAST HOOK (PLUG-IN)
    // =========================
    // If you have a RayManager or physics system, replace the placeholder below
    // with a call to that system to perform a raycast and set hitPoint to the
    // returned impact location (and optionally handle hits: apply damage, spawn FX).
    //
    // Example pseudo-code (adjust to your RayManager API):
    //
    // RayHit result;
    // if (RayManager::raycast(rayOrigin, normDirection, maxShootDistance, result)) {
    //     hitPoint = result.point;
    //     // handle hit (damage/effects) using result.entity or result.collider
    //     std::cout << "Hit object id: " << result.id << "\n";
    // } else {
    //     std::cout << "Raycast hit nothing; bullet traveled to max distance.\n";
    // }
    //
    // =========================

    // Visual / audio effects, recoil, etc. should be triggered here or via a callback.
    std::cout << "Fired. Endpoint: (" << hitPoint.x << ", " << hitPoint.y << ", " << hitPoint.z << ")\n";
}

void Gun::render(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale, Shader& shader) const {
    if (gunModel) {
        gunModel->draw(position, rotation, scale, shader);
    }
}

void Gun::reload() noexcept {
    if (isReloading || currentAmmo >= maxAmmo) {
        return;
    }
    isReloading = true;
    reloadTimer = 0.0f;
    std::cout << "Reloading...\n";
}

bool Gun::loadModel(const std::string& path) {
    try {
        auto m = std::make_unique<Model>(path);
        gunModel = std::move(m);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to load model '" << path << "': " << e.what() << "\n";
        gunModel.reset();
        return false;
    }
}

void Gun::tryFireIfReady(const glm::vec3& rayOrigin, const glm::vec3& rayDirection) {
    if (!wantsToFire) return;
    if (isReloading) return;
    if (timeSinceLastShot < fireInterval) return;
    if (currentAmmo == 0) {
        reload();
        return;
    }

    fire(rayOrigin, rayDirection);

    timeSinceLastShot = 0.0f;
    if (currentAmmo > 0) --currentAmmo;

    if (currentAmmo == 0) {
        reload();
    }
}
