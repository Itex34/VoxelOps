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

struct ChunkRequest {
    int32_t chunkX = 0;
    int32_t chunkY = 0;
    int32_t chunkZ = 0;
    uint16_t viewDistance = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<ChunkRequest> deserialize(const std::vector<uint8_t>& buf);
};

struct ChunkData {
    int32_t chunkX = 0;
    int32_t chunkY = 0;
    int32_t chunkZ = 0;
    uint64_t version = 0;
    uint8_t flags = 0; // bit0: compressed
    std::vector<uint8_t> payload;

    std::vector<uint8_t> serialize() const;
    static std::optional<ChunkData> deserialize(const std::vector<uint8_t>& buf);
};

struct ChunkDeltaOp {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t z = 0;
    uint8_t blockId = 0;
};

struct ChunkDelta {
    int32_t chunkX = 0;
    int32_t chunkY = 0;
    int32_t chunkZ = 0;
    uint64_t resultingVersion = 0;
    std::vector<ChunkDeltaOp> edits;

    std::vector<uint8_t> serialize() const;
    static std::optional<ChunkDelta> deserialize(const std::vector<uint8_t>& buf);
};

struct ChunkUnload {
    int32_t chunkX = 0;
    int32_t chunkY = 0;
    int32_t chunkZ = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<ChunkUnload> deserialize(const std::vector<uint8_t>& buf);
};

struct ChunkAck {
    uint8_t ackedType = 0;
    uint32_t sequence = 0;
    int32_t chunkX = 0;
    int32_t chunkY = 0;
    int32_t chunkZ = 0;
    uint64_t version = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<ChunkAck> deserialize(const std::vector<uint8_t>& buf);
};
