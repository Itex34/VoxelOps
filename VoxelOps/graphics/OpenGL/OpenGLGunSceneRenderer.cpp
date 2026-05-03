#include "OpenGLGunSceneRenderer.hpp"

#include "../../application/AppHelpers.hpp"
#include "../../runtime/Runtime.hpp"
#include "../../player/Player.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

using namespace AppHelpers;

void OpenGLGunSceneRenderer::renderRemotePlayerGuns(
    Runtime &runtime, const Camera &activeCamera, const glm::vec3 &sunDirection
) {
    if (!runtime.render.gunRenderer || runtime.combat.preloadedGuns.empty() ||
        runtime.gameplay.player->connectedPlayers.empty()) {
        return;
    }

    constexpr float kLocalGhostRejectDistance = 2.0f;
    const float localGhostRejectDistanceSq = kLocalGhostRejectDistance * kLocalGhostRejectDistance;
    constexpr float kMinRemoteGunDistanceFromCamera = 2.5f;
    const float minRemoteGunDistanceSq =
        kMinRemoteGunDistanceFromCamera * kMinRemoteGunDistanceFromCamera;

    const float aspect =
        static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight);
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return;
    }

    const glm::mat4 projection =
        glm::perspective(glm::radians(GameData::FOV), aspect, 0.1f, 100000.0f);
    const glm::mat4 view = activeCamera.getViewMatrix();

    for (const auto &[_, remoteState] : runtime.gameplay.player->connectedPlayers) {
        const glm::vec3 toLocal = remoteState.position - runtime.gameplay.player->getPosition();
        const float localDistSq = glm::dot(toLocal, toLocal);
        if (!std::isfinite(localDistSq) || localDistSq < localGhostRejectDistanceSq) {
            continue;
        }

        const uint16_t weaponId = remoteState.weaponId;
        auto gunIt = runtime.combat.preloadedGuns.find(weaponId);
        if (gunIt == runtime.combat.preloadedGuns.end() || !gunIt->second ||
            !runtime.render.gunRenderer->hasWeaponModel(weaponId)) {
            continue;
        }

        const GunDefinition *definition = FindGunDefinitionByWeaponId(weaponId);
        if (definition == nullptr) {
            continue;
        }

        const glm::vec3 handAnchorPos =
            remoteState.position + (remoteState.rotation * kRemoteGunRightHandAnchorOffset);
        const glm::vec3 worldOffset = definition->worldOffset * remoteState.scale;
        const glm::vec3 gunPos = handAnchorPos + (remoteState.rotation * worldOffset);
        const glm::vec3 toCamera = gunPos - activeCamera.position;
        const float distSq = glm::dot(toCamera, toCamera);
        if (!std::isfinite(distSq) || distSq < minRemoteGunDistanceSq) {
            continue;
        }

        const glm::quat yawOffset =
            glm::angleAxis(glm::radians(definition->worldEulerDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::quat ownerYawCorrection = glm::angleAxis(
            glm::radians(kRemoteGunOwnerYawCorrectionDeg), glm::vec3(0.0f, 1.0f, 0.0f)
        );
        const glm::quat pitchOffset =
            glm::angleAxis(glm::radians(definition->worldEulerDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::quat rollOffset =
            glm::angleAxis(glm::radians(definition->worldEulerDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::quat gunRot = glm::normalize(
            remoteState.rotation * ownerYawCorrection * yawOffset * pitchOffset * rollOffset
        );
        const glm::vec3 gunScale = definition->worldScale * remoteState.scale;

        (void)runtime.render.gunRenderer->renderWorldWeapon(
            weaponId,
            gunPos,
            gunRot,
            gunScale,
            view,
            projection,
            glm::normalize(sunDirection),
            glm::vec3(1.0f, 0.98f, 0.96f),
            glm::vec3(0.36f, 0.40f, 0.46f)
        );
    }
}

void OpenGLGunSceneRenderer::renderHeldGun(
    Runtime &runtime, const Camera &activeCamera, const glm::vec3 &sunDirection
) {
    if (!runtime.render.gunRenderer || !runtime.combat.equippedGun) {
        return;
    }

    const uint16_t heldWeaponId = runtime.combat.equippedGun->getWeaponId();
    const float frontLenSq = glm::dot(activeCamera.front, activeCamera.front);
    if (!std::isfinite(frontLenSq) || frontLenSq < 1e-8f) {
        return;
    }
    glm::vec3 forward = glm::normalize(activeCamera.front);
    glm::vec3 up = activeCamera.up;
    const float upLenSq = glm::dot(up, up);
    if (!std::isfinite(upLenSq) || upLenSq < 1e-8f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    } else {
        up = glm::normalize(up);
    }
    glm::vec3 right = glm::cross(forward, up);
    const float rightLenSq = glm::dot(right, right);
    if (!std::isfinite(rightLenSq) || rightLenSq < 1e-8f) {
        right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
        const float fallbackLenSq = glm::dot(right, right);
        if (!std::isfinite(fallbackLenSq) || fallbackLenSq < 1e-8f) {
            return;
        }
    }
    right = glm::normalize(right);
    up = glm::normalize(glm::cross(right, forward));

    const glm::vec3 gunPos = activeCamera.position +
                             right * runtime.combat.equippedGunViewOffset.x +
                             up * runtime.combat.equippedGunViewOffset.y +
                             forward * runtime.combat.equippedGunViewOffset.z;

    const glm::mat4 lookBasis = glm::inverse(glm::lookAt(glm::vec3(0.0f), forward, up));
    glm::quat gunRot = glm::normalize(glm::quat_cast(glm::mat3(lookBasis)));
    const glm::quat yawOffset = glm::angleAxis(
        glm::radians(runtime.combat.equippedGunViewEulerDeg.y), glm::vec3(0.0f, 1.0f, 0.0f)
    );
    const glm::quat pitchOffset = glm::angleAxis(
        glm::radians(runtime.combat.equippedGunViewEulerDeg.x), glm::vec3(1.0f, 0.0f, 0.0f)
    );
    const glm::quat rollOffset = glm::angleAxis(
        glm::radians(runtime.combat.equippedGunViewEulerDeg.z), glm::vec3(0.0f, 0.0f, 1.0f)
    );
    gunRot = glm::normalize(gunRot * yawOffset * pitchOffset * rollOffset);

    const float aspect =
        static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight);
    const glm::mat4 projection =
        glm::perspective(glm::radians(GameData::FOV), aspect, 0.02f, 200.0f);
    const glm::mat4 view = activeCamera.getViewMatrix();

    (void)runtime.render.gunRenderer->renderViewWeapon(
        heldWeaponId,
        gunPos,
        gunRot,
        runtime.combat.equippedGunViewScale,
        view,
        projection,
        glm::normalize(sunDirection),
        glm::vec3(1.0f, 0.98f, 0.96f),
        glm::vec3(0.42f, 0.44f, 0.47f)
    );
}


