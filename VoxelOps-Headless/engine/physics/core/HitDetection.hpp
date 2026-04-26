#pragma once

#include "../../../game/combat/LagCompensation.hpp"
#include "../../../game/player/Hitbox.hpp"
#include "../../../game/player/ServerPlayer.hpp"

#include <glm/vec3.hpp>

#include <vector>

class ChunkManager;

namespace HitDetection {

struct HitDetectionInput {
    PlayerID shooterId = 0;
    glm::vec3 rayOrigin{0.0f};
    glm::vec3 rayDir{0.0f};
    float maxDistance = 128.0f;
    float hitboxPadXZ = 0.08f;
    float hitboxPadY = 0.04f;
    const ChunkManager *chunkManager = nullptr;
    const LagCompensation::LagCompFrame *lagCompFrame = nullptr;
    const std::vector<ServerPlayerCombatSnapshot> *players = nullptr;
    bool enableValidationLogs = false;
};

struct HitDetectionResult {
    bool playerHit = false;
    PlayerID hitPlayerId = 0;
    HitRegion hitRegion = HitRegion::Unknown;
    glm::vec3 hitPoint{0.0f};
    float bestPlayerDistance = 0.0f;
    float maxDistance = 0.0f;
    bool blockHit = false;
    float blockDistance = 0.0f;
    glm::vec3 blockHitPoint{0.0f};
};

HitDetectionResult RaycastPlayersAndWorld(const HitDetectionInput &input);

} // namespace HitDetection
