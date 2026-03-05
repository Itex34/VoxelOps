#pragma once
#include "PacketType.hpp"
#include <vector>
#include <cstdint>
#include <optional>
#include <string>

/// Shared network packet definitions used by client and server.
/// Binary layout uses little-endian. First byte of any packet is PacketType.

constexpr uint8_t kPlayerInputFlagForward = 1u << 0;
constexpr uint8_t kPlayerInputFlagBackward = 1u << 1;
constexpr uint8_t kPlayerInputFlagLeft = 1u << 2;
constexpr uint8_t kPlayerInputFlagRight = 1u << 3;
constexpr uint8_t kPlayerInputFlagJump = 1u << 4;
constexpr uint8_t kPlayerInputFlagSprint = 1u << 5;
constexpr uint8_t kPlayerInputFlagFlyUp = 1u << 6;
constexpr uint8_t kPlayerInputFlagFlyDown = 1u << 7;

constexpr uint16_t kVoxelOpsProtocolVersion = 2;
constexpr size_t kMaxConnectIdentityChars = 64;
constexpr size_t kMaxConnectUsernameChars = 32;
constexpr size_t kMaxConnectMessageChars = 120;

enum class ConnectRejectReason : uint8_t {
    None = 0,
    ProtocolMismatch = 1,
    InvalidPacket = 2,
    InvalidIdentity = 3,
    IdentityInUse = 4,
    ServerError = 5,
    UsernameTaken = 6
};

struct ConnectRequest {
    uint16_t protocolVersion = kVoxelOpsProtocolVersion;
    std::string identity;
    std::string requestedUsername;

    std::vector<uint8_t> serialize() const;
    static std::optional<ConnectRequest> deserialize(const std::vector<uint8_t>& buf);
};

struct ConnectResponse {
    uint8_t ok = 0;
    ConnectRejectReason reason = ConnectRejectReason::None;
    uint16_t serverProtocolVersion = kVoxelOpsProtocolVersion;
    std::string assignedUsername;
    std::string message;

    std::vector<uint8_t> serialize() const;
    static std::optional<ConnectResponse> deserialize(const std::vector<uint8_t>& buf);
};

struct PlayerInput {
    uint32_t sequenceNumber = 0;
    uint8_t inputFlags = 0;
    uint8_t flyMode = 0;
    float yaw = 0.f;
    float pitch = 0.f;
    float moveX = 0.f;
    float moveZ = 0.f;

    std::vector<uint8_t> serialize() const;
    static std::optional<PlayerInput> deserialize(const std::vector<uint8_t>& buf);
};

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
    uint64_t id = 0;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    uint8_t onGround = 0;
    uint8_t flyMode = 0;
    uint8_t allowFlyMode = 0;
};

struct PlayerSnapshotFrame {
    uint32_t serverTick = 0;
    uint64_t selfPlayerId = 0;
    uint32_t lastProcessedInputSequence = 0;
    std::vector<PlayerSnapshot> players;

    std::vector<uint8_t> serialize() const;
    static std::optional<PlayerSnapshotFrame> deserialize(const std::vector<uint8_t>& buf);
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
