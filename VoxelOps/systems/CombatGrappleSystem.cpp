#include "CombatGrappleSystem.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"
#include "../graphics/Camera.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"
#include "../../Shared/player/GrappleSwing.hpp"

#include <SDL3/SDL.h>

#include <glm/geometric.hpp>

#include <cmath>

namespace {
    bool IsMouseButtonDown(uint8_t button) {
        return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
    }

    glm::vec3 ComputeViewmodelMuzzlePosition(const Camera &camera, const RuntimeCombatState &combat) {
        glm::vec3 forward = camera.front;
        const float forwardLenSq = glm::dot(forward, forward);
        if (!std::isfinite(forwardLenSq) || forwardLenSq < 1e-8f) {
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        } else {
            forward = glm::normalize(forward);
        }

        glm::vec3 up = camera.up;
        const float upLenSq = glm::dot(up, up);
        if (!std::isfinite(upLenSq) || upLenSq < 1e-8f) {
            up = glm::vec3(0.0f, 1.0f, 0.0f);
        } else {
            up = glm::normalize(up);
        }

        glm::vec3 right = glm::cross(forward, up);
        const float rightLenSq = glm::dot(right, right);
        if (!std::isfinite(rightLenSq) || rightLenSq < 1e-8f) {
            right = glm::vec3(1.0f, 0.0f, 0.0f);
        } else {
            right = glm::normalize(right);
        }
        up = glm::normalize(glm::cross(right, forward));

        const glm::vec3 gunPos = camera.position + right * combat.equippedGunViewOffset.x +
                                 up * combat.equippedGunViewOffset.y +
                                 forward * combat.equippedGunViewOffset.z;
        return gunPos + (forward * 0.70f);
    }

    bool IsGrappleItemEquipped(const Runtime &runtime) {
        if (!runtime.ui.inventoryUi || !runtime.ui.inventoryUi->hasSnapshot()) {
            return false;
        }
        if (runtime.combat.activeHotbarSlot >= static_cast<uint16_t>(kHotbarSlots)) {
            return false;
        }
        const Slot &activeSlot = runtime.ui.inventoryUi->slots()[runtime.combat.activeHotbarSlot];
        return activeSlot.itemId == static_cast<uint16_t>(ITEM_GRAPPLE_GUN) && activeSlot.quantity > 0;
    }
} // namespace

void CombatGrappleSystem::update(Runtime &runtime, const CombatGrappleSystemContext &ctx) {
    RuntimeCombatState::GrappleRuntimeState &grapple = runtime.combat.grapple;
    const bool fireDown = IsMouseButtonDown(SDL_BUTTON_LEFT);
    const bool releaseDown = IsMouseButtonDown(SDL_BUTTON_RIGHT);
    const bool releasePressed = releaseDown && !grapple.wasReleaseDown;
    const bool hasGrappleEquipped = IsGrappleItemEquipped(runtime);

    if (!runtime.combat.localPlayerAlive) {
        grapple.wasFireDown = fireDown;
        grapple.wasReleaseDown = releaseDown;
        grapple.isAttached = false;
        grapple.ropeLength = 0.0f;
        return;
    }

    if (GameData::cursorEnabled || ctx.useDebugCamera || !runtime.network.clientNet.IsConnected()) {
        grapple.wasFireDown = fireDown;
        grapple.wasReleaseDown = releaseDown;
        return;
    }

    const double now = AppHelpers::GetTimeSeconds();
    const uint32_t clientTick =
        runtime.prediction.hasAppliedServerTick ? runtime.prediction.lastAppliedServerTick : 0u;

    if (grapple.isAttached && releasePressed) {
        const uint32_t grappleId = grapple.nextClientGrappleId++;
        const uint32_t seed = grappleId ^ (clientTick * 3266489917u);
        if (runtime.network.clientNet.SendGrappleRequest(
                grappleId,
                clientTick,
                glm::vec3(0.0f),
                glm::vec3(0.0f),
                seed
            )) {
            grapple.pendingControlRequestIds.insert(grappleId);
            grapple.isAttached = false;
            grapple.ropeLength = 0.0f;
            grapple.anchorFaceNormal = 255;
        }
    }

    if (grapple.isAttached && fireDown) {
        Shared::Grapple::ApplyReelIn(grapple.ropeLength, static_cast<float>(GameData::deltaTime));
        if ((now - grapple.lastReelSendTime) >= grapple.reelSendInterval) {
            const uint32_t grappleId = grapple.nextClientGrappleId++;
            const uint32_t seed = grappleId ^ (clientTick * 3266489917u);
            if (runtime.network.clientNet.SendGrappleRequest(
                    grappleId,
                    clientTick,
                    glm::vec3(0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f),
                    seed
                )) {
                grapple.pendingControlRequestIds.insert(grappleId);
                grapple.lastReelSendTime = now;
            }
        }
    }

    if (grapple.isAttached) {
        grapple.wasFireDown = fireDown;
        grapple.wasReleaseDown = releaseDown;
        return;
    }

    const bool firePressed = fireDown && !grapple.wasFireDown;
    if (!hasGrappleEquipped || !firePressed) {
        grapple.wasFireDown = fireDown;
        grapple.wasReleaseDown = releaseDown;
        return;
    }

    if ((now - grapple.lastSendTime) < grapple.sendInterval) {
        grapple.wasFireDown = fireDown;
        grapple.wasReleaseDown = releaseDown;
        return;
    }

    const Camera &camera = runtime.gameplay.player->getCamera();
    const float dirLenSq = glm::dot(camera.front, camera.front);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        grapple.wasFireDown = fireDown;
        grapple.wasReleaseDown = releaseDown;
        return;
    }

    const glm::vec3 grappleDir = glm::normalize(camera.front);
    const glm::vec3 grapplePos = ComputeViewmodelMuzzlePosition(camera, runtime.combat);
    const uint32_t grappleId = grapple.nextClientGrappleId++;
    const uint32_t seed = grappleId ^ (clientTick * 2246822519u);

    if (runtime.network.clientNet.SendGrappleRequest(
            grappleId,
            clientTick,
            grapplePos,
            grappleDir,
            seed
        )) {
        grapple.lastSendTime = now;
        grapple.hasLastLocalFire = true;
        grapple.lastLocalFireId = grappleId;
        grapple.lastLocalFireOrigin = grapplePos;
        grapple.lastLocalFireDirection = grappleDir;
        grapple.lastLocalFireTime = now;
    }

    grapple.wasFireDown = fireDown;
    grapple.wasReleaseDown = releaseDown;
}
