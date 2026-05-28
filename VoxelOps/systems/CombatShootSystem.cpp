#include "CombatShootSystem.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"

#include <SDL3/SDL.h>

#include <glm/geometric.hpp>

#include <cmath>

namespace {
    bool IsMouseButtonDown(uint8_t button) {
        return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
    }
} // namespace

void CombatShootSystem::update(Runtime &runtime, const CombatShootSystemContext &ctx) {
    if (!runtime.combat.localPlayerAlive) {
        return;
    }

    if (GameData::cursorEnabled || ctx.useDebugCamera) {
        return;
    }
    if (!runtime.network.clientNet.IsConnected()) {
        return;
    }
    if (!runtime.combat.equippedGun) {
        return;
    }

    const bool triggerPressed = IsMouseButtonDown(SDL_BUTTON_LEFT);
    if (!triggerPressed) {
        return;
    }

    const double now = AppHelpers::GetTimeSeconds();
    if ((now - runtime.combat.lastShootSendTime) < runtime.combat.shootSendInterval) {
        return;
    }

    const Camera &cam = runtime.gameplay.player->getCamera();
    const float dirLenSq = glm::dot(cam.front, cam.front);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        return;
    }

    const glm::vec3 shootDir = glm::normalize(cam.front);
    const glm::vec3 shootPos = cam.position;
    const uint32_t shotId = runtime.combat.nextClientShotId++;
    const uint32_t clientTick =
        (runtime.prediction.inputTickCounter > 0) ? (runtime.prediction.inputTickCounter - 1u) : 0u;
    const uint32_t seed = shotId ^ (clientTick * 2654435761u);

    if (runtime.network.clientNet.SendShootRequest(
            shotId,
            clientTick,
            runtime.combat.equippedGun->getWeaponId(),
            shootPos,
            shootDir,
            seed,
            0
        )) {
        glm::vec3 up = cam.up;
        const float upLenSq = glm::dot(up, up);
        if (!std::isfinite(upLenSq) || upLenSq < 1e-8f) {
            up = glm::vec3(0.0f, 1.0f, 0.0f);
        } else {
            up = glm::normalize(up);
        }
        glm::vec3 right = glm::cross(shootDir, up);
        const float rightLenSq = glm::dot(right, right);
        if (!std::isfinite(rightLenSq) || rightLenSq < 1e-8f) {
            right = glm::cross(shootDir, glm::vec3(0.0f, 1.0f, 0.0f));
            const float fallbackLenSq = glm::dot(right, right);
            if (std::isfinite(fallbackLenSq) && fallbackLenSq >= 1e-8f) {
                right = glm::normalize(right);
            } else {
                right = glm::vec3(1.0f, 0.0f, 0.0f);
            }
        } else {
            right = glm::normalize(right);
        }
        up = glm::normalize(glm::cross(right, shootDir));

        const glm::vec3 gunPos = shootPos + right * runtime.combat.equippedGunViewOffset.x +
                                 up * runtime.combat.equippedGunViewOffset.y +
                                 shootDir * runtime.combat.equippedGunViewOffset.z;
        const glm::vec3 muzzlePos = gunPos + (shootDir * 0.70f);

        runtime.combat.lastShootSendTime = now;
        runtime.combat.hasLastLocalShot = true;
        runtime.combat.lastLocalShotId = shotId;
        runtime.combat.lastLocalShotOrigin = muzzlePos;
        runtime.combat.lastLocalShotDirection = shootDir;
        runtime.combat.lastLocalShotTime = now;
    }
}
