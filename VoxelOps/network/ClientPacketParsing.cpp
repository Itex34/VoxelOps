#include "ClientPacketParsing.hpp"

#include <cstring>
#include <optional>
#include <vector>

namespace {

bool ReadU8(std::span<const uint8_t> bytes, size_t &offset, uint8_t &out) {
    if (offset + 1 > bytes.size()) {
        return false;
    }
    out = bytes[offset++];
    return true;
}

bool ReadU16LE(std::span<const uint8_t> bytes, size_t &offset, uint16_t &out) {
    if (offset + 2 > bytes.size()) {
        return false;
    }
    out = static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
    offset += 2;
    return true;
}

bool ReadU32LE(std::span<const uint8_t> bytes, size_t &offset, uint32_t &out) {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    out = static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
          (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
          (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool ReadI32LE(std::span<const uint8_t> bytes, size_t &offset, int32_t &out) {
    uint32_t raw = 0;
    if (!ReadU32LE(bytes, offset, raw)) {
        return false;
    }
    out = static_cast<int32_t>(raw);
    return true;
}

bool ReadU64LE(std::span<const uint8_t> bytes, size_t &offset, uint64_t &out) {
    if (offset + 8 > bytes.size()) {
        return false;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= (static_cast<uint64_t>(bytes[offset + i]) << (8 * i));
    }
    out = value;
    offset += 8;
    return true;
}

bool ReadF32LE(std::span<const uint8_t> bytes, size_t &offset, float &out) {
    uint32_t raw = 0;
    if (!ReadU32LE(bytes, offset, raw)) {
        return false;
    }
    std::memcpy(&out, &raw, sizeof(out));
    return true;
}

} // namespace

namespace ClientPackets {

bool ParseConnectResponse(std::span<const uint8_t> bytes, ConnectResponse &out) {
    size_t offset = 0;
    uint8_t type = 0;
    uint8_t ok = 0;
    uint8_t reason = 0;
    uint8_t assignedLen = 0;
    uint8_t messageLen = 0;
    uint16_t protocolVersion = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::ConnectResponse) ||
        !ReadU8(bytes, offset, ok) || !ReadU8(bytes, offset, reason) ||
        !ReadU16LE(bytes, offset, protocolVersion) || !ReadU8(bytes, offset, assignedLen) ||
        !ReadU8(bytes, offset, messageLen)) {
        return false;
    }
    if (assignedLen > kMaxConnectUsernameChars || messageLen > kMaxConnectMessageChars) {
        return false;
    }
    if (offset + static_cast<size_t>(assignedLen) + static_cast<size_t>(messageLen) != bytes.size()) {
        return false;
    }

    out.ok = (ok != 0) ? 1u : 0u;
    out.reason = static_cast<ConnectRejectReason>(reason);
    out.serverProtocolVersion = protocolVersion;
    out.assignedUsername.assign(reinterpret_cast<const char *>(bytes.data() + offset), assignedLen);
    offset += assignedLen;
    out.message.assign(reinterpret_cast<const char *>(bytes.data() + offset), messageLen);
    return true;
}

bool ParsePlayerSnapshotFrame(std::span<const uint8_t> bytes, PlayerSnapshotFrame &out) {
    constexpr size_t kSnapshotEntryBytes = 8 + (8 * 4) + 3 + 2 + 4 + 1 + 4 + 1 + 4 + 4 + 4;
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t playerCount = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::PlayerSnapshot) ||
        !ReadU32LE(bytes, offset, out.serverTick) || !ReadU64LE(bytes, offset, out.selfPlayerId) ||
        !ReadU32LE(bytes, offset, out.lastProcessedInputTick) ||
        !ReadU32LE(bytes, offset, playerCount)) {
        return false;
    }

    const size_t bytesRemaining = (offset <= bytes.size()) ? (bytes.size() - offset) : 0;
    if (playerCount > (bytesRemaining / kSnapshotEntryBytes)) {
        return false;
    }

    out.players.clear();
    out.players.reserve(playerCount);
    for (uint32_t i = 0; i < playerCount; ++i) {
        PlayerSnapshot snapshot{};
        if (!ReadU64LE(bytes, offset, snapshot.id) || !ReadF32LE(bytes, offset, snapshot.px) ||
            !ReadF32LE(bytes, offset, snapshot.py) || !ReadF32LE(bytes, offset, snapshot.pz) ||
            !ReadF32LE(bytes, offset, snapshot.vx) || !ReadF32LE(bytes, offset, snapshot.vy) ||
            !ReadF32LE(bytes, offset, snapshot.vz) || !ReadF32LE(bytes, offset, snapshot.yaw) ||
            !ReadF32LE(bytes, offset, snapshot.pitch) || !ReadU8(bytes, offset, snapshot.onGround) ||
            !ReadU8(bytes, offset, snapshot.flyMode) ||
            !ReadU8(bytes, offset, snapshot.allowFlyMode) ||
            !ReadU16LE(bytes, offset, snapshot.weaponId) ||
            !ReadF32LE(bytes, offset, snapshot.health) || !ReadU8(bytes, offset, snapshot.isAlive) ||
            !ReadF32LE(bytes, offset, snapshot.respawnSeconds) ||
            !ReadU8(bytes, offset, snapshot.jumpPressedLastTick) ||
            !ReadF32LE(bytes, offset, snapshot.timeSinceGrounded) ||
            !ReadF32LE(bytes, offset, snapshot.jumpBufferTimer) ||
            !ReadF32LE(bytes, offset, snapshot.stepCooldownTimer)) {
            return false;
        }
        out.players.push_back(snapshot);
    }

    return offset == bytes.size();
}

bool ParseChunkData(std::span<const uint8_t> bytes, ChunkData &out) {
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t payloadSize = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::ChunkData) ||
        !ReadI32LE(bytes, offset, out.chunkX) || !ReadI32LE(bytes, offset, out.chunkY) ||
        !ReadI32LE(bytes, offset, out.chunkZ) || !ReadU64LE(bytes, offset, out.version) ||
        !ReadU8(bytes, offset, out.flags) || !ReadU32LE(bytes, offset, payloadSize)) {
        return false;
    }
    if (offset + payloadSize != bytes.size()) {
        return false;
    }
    out.payload.resize(payloadSize);
    if (payloadSize > 0) {
        std::memcpy(out.payload.data(), bytes.data() + offset, payloadSize);
    }
    return true;
}

bool ParseChunkDelta(std::span<const uint8_t> bytes, ChunkDelta &out) {
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t editCount = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::ChunkDelta) ||
        !ReadI32LE(bytes, offset, out.chunkX) || !ReadI32LE(bytes, offset, out.chunkY) ||
        !ReadI32LE(bytes, offset, out.chunkZ) || !ReadU64LE(bytes, offset, out.resultingVersion) ||
        !ReadU32LE(bytes, offset, editCount)) {
        return false;
    }
    if (editCount > ((bytes.size() - offset) / 4)) {
        return false;
    }

    out.edits.clear();
    out.edits.reserve(editCount);
    for (uint32_t i = 0; i < editCount; ++i) {
        ChunkDeltaOp op{};
        if (!ReadU8(bytes, offset, op.x) || !ReadU8(bytes, offset, op.y) ||
            !ReadU8(bytes, offset, op.z) || !ReadU8(bytes, offset, op.blockId)) {
            return false;
        }
        out.edits.push_back(op);
    }
    return offset == bytes.size();
}

bool ParseChunkUnload(std::span<const uint8_t> bytes, ChunkUnload &out) {
    size_t offset = 0;
    uint8_t type = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::ChunkUnload) ||
        !ReadI32LE(bytes, offset, out.chunkX) || !ReadI32LE(bytes, offset, out.chunkY) ||
        !ReadI32LE(bytes, offset, out.chunkZ)) {
        return false;
    }
    return offset == bytes.size();
}

bool ParseShootResult(std::span<const uint8_t> bytes, ShootResult &out) {
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t entityRaw = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::ShootResult) ||
        !ReadU32LE(bytes, offset, out.clientShotId) || !ReadU32LE(bytes, offset, out.serverTick) ||
        !ReadU8(bytes, offset, out.accepted) || !ReadU8(bytes, offset, out.didHit) ||
        !ReadU32LE(bytes, offset, entityRaw) || !ReadF32LE(bytes, offset, out.hitX) ||
        !ReadF32LE(bytes, offset, out.hitY) || !ReadF32LE(bytes, offset, out.hitZ) ||
        !ReadF32LE(bytes, offset, out.normalX) || !ReadF32LE(bytes, offset, out.normalY) ||
        !ReadF32LE(bytes, offset, out.normalZ) || !ReadF32LE(bytes, offset, out.damageApplied) ||
        !ReadU16LE(bytes, offset, out.newAmmoCount) || !ReadU32LE(bytes, offset, out.serverSeed)) {
        return false;
    }
    out.hitEntityId = static_cast<int32_t>(entityRaw);
    return offset == bytes.size();
}

bool ParseGrappleResult(std::span<const uint8_t> bytes, GrappleResult &out) {
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t entityRaw = 0;
    if (!ReadU8(bytes, offset, type) || type != static_cast<uint8_t>(PacketType::GrappleResult) ||
        !ReadU32LE(bytes, offset, out.clientGrappleId) ||
        !ReadU32LE(bytes, offset, out.serverTick) || !ReadU8(bytes, offset, out.accepted) ||
        !ReadU8(bytes, offset, out.didHit) || !ReadU32LE(bytes, offset, entityRaw) ||
        !ReadF32LE(bytes, offset, out.hitX) || !ReadF32LE(bytes, offset, out.hitY) ||
        !ReadF32LE(bytes, offset, out.hitZ) || !ReadU8(bytes, offset, out.faceNormal) ||
        !ReadU32LE(bytes, offset, out.serverSeed)) {
        return false;
    }
    out.hitEntityId = static_cast<int32_t>(entityRaw);
    return offset == bytes.size();
}

bool ParseInventoryActionResult(std::span<const uint8_t> bytes, InventoryActionResult &out) {
    if (bytes.empty()) {
        return false;
    }
    const std::vector<uint8_t> owned(bytes.begin(), bytes.end());
    const std::optional<InventoryActionResult> parsed = InventoryActionResult::deserialize(owned);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ParseInventorySnapshot(std::span<const uint8_t> bytes, InventorySnapshot &out) {
    if (bytes.empty()) {
        return false;
    }
    const std::vector<uint8_t> owned(bytes.begin(), bytes.end());
    const std::optional<InventorySnapshot> parsed = InventorySnapshot::deserialize(owned);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ParseWorldItemSnapshot(std::span<const uint8_t> bytes, WorldItemSnapshot &out) {
    if (bytes.empty()) {
        return false;
    }
    const std::vector<uint8_t> owned(bytes.begin(), bytes.end());
    const std::optional<WorldItemSnapshot> parsed = WorldItemSnapshot::deserialize(owned);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ParseBlockPlaceResult(std::span<const uint8_t> bytes, BlockPlaceResult &out) {
    if (bytes.empty()) {
        return false;
    }
    const std::vector<uint8_t> owned(bytes.begin(), bytes.end());
    const std::optional<BlockPlaceResult> parsed = BlockPlaceResult::deserialize(owned);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ParseBlockBreakResult(std::span<const uint8_t> bytes, BlockBreakResult &out) {
    if (bytes.empty()) {
        return false;
    }
    const std::vector<uint8_t> owned(bytes.begin(), bytes.end());
    const std::optional<BlockBreakResult> parsed = BlockBreakResult::deserialize(owned);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

} // namespace ClientPackets
