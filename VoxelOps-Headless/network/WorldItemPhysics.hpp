#pragma once

#include <glm/vec3.hpp>
#include <cstdint>

class ChunkManager;

struct WorldItemEntity {
    uint64_t id = 0;
    uint16_t itemId = 0;
    uint16_t quantity = 0;
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    float pickupCooldownSeconds = 0.0f;
    float ttlSeconds = 0.0f;
};

class WorldItemPhysicsSystem {
public:
    static constexpr float kGravity = 24.0f;
    static constexpr float kAirDampingPerTick = 0.995f;
    static constexpr float kGroundDampingPerTick = 0.90f;
    static constexpr float kPickupRadius = 1.35f;
    static constexpr float kPickupCooldownSeconds = 0.35f;
    static constexpr float kTtlSeconds = 120.0f;
    static constexpr float kMaxSpeed = 10.0f;
    static constexpr float kCollisionRadius = 0.17f;
    static constexpr float kCollisionHeight = 0.22f;

    static void Step(
        WorldItemEntity& item,
        float deltaSeconds,
        float tickRateHz,
        const ChunkManager& chunkManager
    );
};
