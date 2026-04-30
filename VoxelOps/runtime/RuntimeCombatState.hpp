#pragma once

#include "../gun/Gun.hpp"
#include "../../Shared/gun/GunType.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

struct RuntimeCombatState {
    bool localPlayerAlive = true;
    float localHealth = 100.0f;
    float localRespawnSeconds = 0.0f;
    std::string localDeathKiller;
    bool wasRespawnClickDown = false;

    double lastShootSendTime = 0.0;
    double shootSendInterval = 1.0 / 8.0;
    uint32_t nextClientShotId = 1;

    uint16_t activeHotbarSlot = 0;
    GunType equippedGunType = kDefaultGunType;
    std::unordered_map<uint16_t, std::unique_ptr<Gun>> preloadedGuns;
    Gun *equippedGun = nullptr;
    glm::vec3 equippedGunViewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
    glm::vec3 equippedGunViewScale = glm::vec3(0.10f);
    glm::vec3 equippedGunViewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
};
