#pragma once
#include "PacketType.hpp"
#include <vector>
#include <cstdint>
#include <optional>

/// Shared network packet definitions used by client and server.
/// Binary layout uses little-endian. First byte of any packet is PacketType.

struct ShootRequest {
    uint32_t clientShotId = 0;
    uint32_t clientTick = 0;
    uint16_t weaponId = 0;
    float    posX = 0.f, posY = 0.f, posZ = 0.f;
    float    dirX = 0.f, dirY = 0.f, dirZ = 0.f;
    uint32_t seed = 0;
    uint8_t  inputFlags = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<ShootRequest> deserialize(const std::vector<uint8_t>& buf);
};

struct ShootResult {
    uint32_t clientShotId = 0;
    uint32_t serverTick = 0;
    uint8_t  accepted = 0;
    uint8_t  didHit = 0;
    int32_t  hitEntityId = -1;
    float    hitX = 0.f, hitY = 0.f, hitZ = 0.f;
    float    normalX = 0.f, normalY = 0.f, normalZ = 0.f;
    float    damageApplied = 0.f;
    uint16_t newAmmoCount = 0;
    uint32_t serverSeed = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<ShootResult> deserialize(const std::vector<uint8_t>& buf);
};



struct PlayerPosition {
    uint32_t sequenceNumber = 0;
    float    posX = 0.f, posY = 0.f, posZ = 0.f;
    float    velX = 0.f, velY = 0.f, velZ = 0.f;
    std::vector<uint8_t> serialize() const;
    static std::optional<PlayerPosition> deserialize(const std::vector<uint8_t>& buf);
};


struct PlayerSnapshot{
    uint64_t id;
    float px, py, pz;
    float vx, vy, vz;
    float yaw;
    float pitch;
    uint8_t onGround;
};