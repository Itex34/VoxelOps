#pragma once

#include "../gun/Gun.hpp"
#include "../../Shared/gun/GunType.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct RuntimeCombatState {
    struct GrappleRuntimeState {
        double lastSendTime = 0.0;
        double sendInterval = 0.12;
        uint32_t nextClientGrappleId = 1;
        bool wasFireDown = false;
        bool wasReleaseDown = false;
        double lastReelSendTime = 0.0;
        double reelSendInterval = 1.0 / 20.0;
        std::unordered_set<uint32_t> pendingControlRequestIds;

        bool hasLastLocalFire = false;
        uint32_t lastLocalFireId = 0;
        glm::vec3 lastLocalFireOrigin = glm::vec3(0.0f);
        glm::vec3 lastLocalFireDirection = glm::vec3(0.0f, 0.0f, -1.0f);
        double lastLocalFireTime = 0.0;

        bool hasAcceptedResult = false;
        uint32_t lastAcceptedResultId = 0;
        glm::vec3 anchorPoint = glm::vec3(0.0f);
        float ropeLength = 0.0f;
        uint8_t anchorFaceNormal = 255;
        double lastAcceptedResultTime = 0.0;
        bool isAttached = false;
    };

    bool localPlayerAlive = true;
    float localHealth = 100.0f;
    float localRespawnSeconds = 0.0f;
    std::string localDeathKiller;
    bool wasRespawnClickDown = false;

    double lastShootSendTime = 0.0;
    double shootSendInterval = 1.0 / 8.0;
    uint32_t nextClientShotId = 1;
    bool hasLastLocalShot = false;
    uint32_t lastLocalShotId = 0;
    glm::vec3 lastLocalShotOrigin = glm::vec3(0.0f);
    glm::vec3 lastLocalShotDirection = glm::vec3(0.0f, 0.0f, -1.0f);
    double lastLocalShotTime = 0.0;
    bool hasLastAcceptedShotResult = false;
    uint32_t lastAcceptedShotResultId = 0;
    glm::vec3 lastAcceptedShotResultPoint = glm::vec3(0.0f);
    double lastAcceptedShotResultTime = 0.0;
    GrappleRuntimeState grapple{};

    uint16_t activeHotbarSlot = 0;
    GunType equippedGunType = kDefaultGunType;
    std::unordered_map<uint16_t, std::unique_ptr<Gun>> preloadedGuns;
    Gun *equippedGun = nullptr;
    glm::vec3 equippedGunViewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
    glm::vec3 equippedGunViewScale = glm::vec3(0.10f);
    glm::vec3 equippedGunViewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
};
