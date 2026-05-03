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
        runtime.prediction.hasAppliedServerTick ? runtime.prediction.lastAppliedServerTick : 0u;
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
        runtime.combat.lastShootSendTime = now;
    }
}
