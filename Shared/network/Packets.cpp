#include "Packets.hpp"
#include <cstring>
#include <cassert>
#include <cmath>
#include <algorithm>

namespace {

    // little-endian writers 
    inline void write_u8(std::vector<uint8_t>& dst, uint8_t v) { dst.push_back(v); }
    inline void write_u16(std::vector<uint8_t>& dst, uint16_t v) {
        dst.push_back(uint8_t(v & 0xFF));
        dst.push_back(uint8_t((v >> 8) & 0xFF));
    }
    inline void write_u32(std::vector<uint8_t>& dst, uint32_t v) {
        dst.push_back(uint8_t((v >> 0) & 0xFF));
        dst.push_back(uint8_t((v >> 8) & 0xFF));
        dst.push_back(uint8_t((v >> 16) & 0xFF));
        dst.push_back(uint8_t((v >> 24) & 0xFF));
    }
    inline void write_i32(std::vector<uint8_t>& dst, int32_t v) {
        write_u32(dst, static_cast<uint32_t>(v));
    }
    inline void write_u64(std::vector<uint8_t>& dst, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            dst.push_back(uint8_t((v >> (8 * i)) & 0xFF));
        }
    }
    inline void write_f32(std::vector<uint8_t>& dst, float v) {
        uint32_t tmp;
        static_assert(sizeof(float) == sizeof(uint32_t));
        std::memcpy(&tmp, &v, sizeof(tmp));
        write_u32(dst, tmp);
    }

    // little-endian readers
    inline bool read_u8(const std::vector<uint8_t>& src, size_t& off, uint8_t& out) {
        if (off + 1 > src.size()) return false;
        out = src[off];
        off += 1;
        return true;
    }
    inline bool read_u16(const std::vector<uint8_t>& src, size_t& off, uint16_t& out) {
        if (off + 2 > src.size()) return false;
        out = uint16_t(src[off]) | (uint16_t(src[off + 1]) << 8);
        off += 2;
        return true;
    }
    inline bool read_u32(const std::vector<uint8_t>& src, size_t& off, uint32_t& out) {
        if (off + 4 > src.size()) return false;
        out = uint32_t(src[off]) | (uint32_t(src[off + 1]) << 8) | (uint32_t(src[off + 2]) << 16) | (uint32_t(src[off + 3]) << 24);
        off += 4;
        return true;
    }
    inline bool read_i32(const std::vector<uint8_t>& src, size_t& off, int32_t& out) {
        uint32_t u = 0;
        if (!read_u32(src, off, u)) return false;
        out = static_cast<int32_t>(u);
        return true;
    }
    inline bool read_u64(const std::vector<uint8_t>& src, size_t& off, uint64_t& out) {
        if (off + 8 > src.size()) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= (uint64_t(src[off + i]) << (8 * i));
        }
        off += 8;
        out = v;
        return true;
    }
    inline bool read_f32(const std::vector<uint8_t>& src, size_t& off, float& out) {
        uint32_t tmp;
        if (!read_u32(src, off, tmp)) return false;
        std::memcpy(&out, &tmp, sizeof(out));
        return true;
    }

} // namespace (internal helpers)




// -------------------- ConnectRequest --------------------
std::vector<uint8_t> ConnectRequest::serialize() const {
    const std::string identityTrimmed = identity.substr(0, std::min(identity.size(), kMaxConnectIdentityChars));
    const std::string nameTrimmed = requestedUsername.substr(0, std::min(requestedUsername.size(), kMaxConnectUsernameChars));

    std::vector<uint8_t> out;
    out.reserve(1 + 2 + 1 + 1 + identityTrimmed.size() + nameTrimmed.size());
    write_u8(out, static_cast<uint8_t>(PacketType::ConnectRequest));
    write_u16(out, protocolVersion);
    write_u8(out, static_cast<uint8_t>(identityTrimmed.size()));
    write_u8(out, static_cast<uint8_t>(nameTrimmed.size()));
    out.insert(out.end(), identityTrimmed.begin(), identityTrimmed.end());
    out.insert(out.end(), nameTrimmed.begin(), nameTrimmed.end());
    return out;
}

std::optional<ConnectRequest> ConnectRequest::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint8_t identityLen = 0;
    uint8_t nameLen = 0;

    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ConnectRequest)) return std::nullopt;

    ConnectRequest req;
    if (!read_u16(buf, off, req.protocolVersion)) return std::nullopt;
    if (!read_u8(buf, off, identityLen)) return std::nullopt;
    if (!read_u8(buf, off, nameLen)) return std::nullopt;
    if (identityLen > kMaxConnectIdentityChars || nameLen > kMaxConnectUsernameChars) return std::nullopt;
    if (off + identityLen + nameLen > buf.size()) return std::nullopt;

    if (identityLen > 0) {
        req.identity.assign(reinterpret_cast<const char*>(buf.data() + off), identityLen);
        off += identityLen;
    }
    if (nameLen > 0) {
        req.requestedUsername.assign(reinterpret_cast<const char*>(buf.data() + off), nameLen);
        off += nameLen;
    }
    if (off != buf.size()) return std::nullopt;
    return req;
}

// -------------------- ConnectResponse --------------------
std::vector<uint8_t> ConnectResponse::serialize() const {
    const std::string assignedTrimmed = assignedUsername.substr(0, std::min(assignedUsername.size(), kMaxConnectUsernameChars));
    const std::string messageTrimmed = message.substr(0, std::min(message.size(), kMaxConnectMessageChars));

    std::vector<uint8_t> out;
    out.reserve(1 + 1 + 1 + 2 + 1 + 1 + assignedTrimmed.size() + messageTrimmed.size());
    write_u8(out, static_cast<uint8_t>(PacketType::ConnectResponse));
    write_u8(out, ok ? 1u : 0u);
    write_u8(out, static_cast<uint8_t>(reason));
    write_u16(out, serverProtocolVersion);
    write_u8(out, static_cast<uint8_t>(assignedTrimmed.size()));
    write_u8(out, static_cast<uint8_t>(messageTrimmed.size()));
    out.insert(out.end(), assignedTrimmed.begin(), assignedTrimmed.end());
    out.insert(out.end(), messageTrimmed.begin(), messageTrimmed.end());
    return out;
}

std::optional<ConnectResponse> ConnectResponse::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint8_t ok = 0;
    uint8_t reason = 0;
    uint8_t assignedLen = 0;
    uint8_t msgLen = 0;

    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ConnectResponse)) return std::nullopt;

    ConnectResponse resp;
    if (!read_u8(buf, off, ok)) return std::nullopt;
    if (!read_u8(buf, off, reason)) return std::nullopt;
    if (!read_u16(buf, off, resp.serverProtocolVersion)) return std::nullopt;
    if (!read_u8(buf, off, assignedLen)) return std::nullopt;
    if (!read_u8(buf, off, msgLen)) return std::nullopt;
    if (assignedLen > kMaxConnectUsernameChars || msgLen > kMaxConnectMessageChars) return std::nullopt;
    if (off + assignedLen + msgLen > buf.size()) return std::nullopt;

    resp.ok = (ok != 0) ? 1u : 0u;
    resp.reason = static_cast<ConnectRejectReason>(reason);
    if (assignedLen > 0) {
        resp.assignedUsername.assign(reinterpret_cast<const char*>(buf.data() + off), assignedLen);
        off += assignedLen;
    }
    if (msgLen > 0) {
        resp.message.assign(reinterpret_cast<const char*>(buf.data() + off), msgLen);
        off += msgLen;
    }
    if (off != buf.size()) return std::nullopt;
    return resp;
}

// -------------------- PlayerInput --------------------
std::vector<uint8_t> PlayerInput::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 1 + 1 + 2 + 4 * 4);
    write_u8(out, static_cast<uint8_t>(PacketType::PlayerInput));
    write_u32(out, inputTick);
    write_u8(out, inputFlags);
    write_u8(out, flyMode);
    write_u16(out, weaponId);
    write_f32(out, yaw);
    write_f32(out, pitch);
    write_f32(out, moveX);
    write_f32(out, moveZ);
    return out;
}

std::optional<PlayerInput> PlayerInput::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::PlayerInput)) return std::nullopt;

    PlayerInput p;
    if (!read_u32(buf, off, p.inputTick)) return std::nullopt;
    if (!read_u8(buf, off, p.inputFlags)) return std::nullopt;
    if (!read_u8(buf, off, p.flyMode)) return std::nullopt;
    if (!read_u16(buf, off, p.weaponId)) return std::nullopt;
    if (!read_f32(buf, off, p.yaw)) return std::nullopt;
    if (!read_f32(buf, off, p.pitch)) return std::nullopt;
    if (!read_f32(buf, off, p.moveX)) return std::nullopt;
    if (!read_f32(buf, off, p.moveZ)) return std::nullopt;
    if (!std::isfinite(p.yaw) || !std::isfinite(p.pitch) || !std::isfinite(p.moveX) || !std::isfinite(p.moveZ)) {
        return std::nullopt;
    }
    return p;
}

std::vector<uint8_t> ShootRequest::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 2 + 12 + 12 + 4 + 1);
    write_u8(out, static_cast<uint8_t>(PacketType::ShootRequest));
    write_u32(out, clientShotId);
    write_u32(out, clientTick);
    write_u16(out, weaponId);
    write_f32(out, posX); write_f32(out, posY); write_f32(out, posZ);
    write_f32(out, dirX); write_f32(out, dirY); write_f32(out, dirZ);
    write_u32(out, seed);
    write_u8(out, inputFlags);
    return out;
}

std::optional<ShootRequest> ShootRequest::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ShootRequest)) return std::nullopt;

    ShootRequest r;
    if (!read_u32(buf, off, r.clientShotId)) return std::nullopt;
    if (!read_u32(buf, off, r.clientTick)) return std::nullopt;
    if (!read_u16(buf, off, r.weaponId)) return std::nullopt;
    if (!read_f32(buf, off, r.posX)) return std::nullopt;
    if (!read_f32(buf, off, r.posY)) return std::nullopt;
    if (!read_f32(buf, off, r.posZ)) return std::nullopt;
    if (!read_f32(buf, off, r.dirX)) return std::nullopt;
    if (!read_f32(buf, off, r.dirY)) return std::nullopt;
    if (!read_f32(buf, off, r.dirZ)) return std::nullopt;
    if (!read_u32(buf, off, r.seed)) return std::nullopt;
    if (!read_u8(buf, off, r.inputFlags)) return std::nullopt;
    if (
        !std::isfinite(r.posX) || !std::isfinite(r.posY) || !std::isfinite(r.posZ) ||
        !std::isfinite(r.dirX) || !std::isfinite(r.dirY) || !std::isfinite(r.dirZ)
    ) {
        return std::nullopt;
    }
    return r;
}

// -------------------- ShootResult --------------------
std::vector<uint8_t> ShootResult::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 1 + 1 + 4 + 4 * 3 + 4 * 3 + 4 + 2 + 4);
    write_u8(out, static_cast<uint8_t>(PacketType::ShootResult));
    write_u32(out, clientShotId);
    write_u32(out, serverTick);
    write_u8(out, accepted);
    write_u8(out, didHit);
    // write signed hitEntityId as u32 bytes
    write_u32(out, static_cast<uint32_t>(hitEntityId));
    write_f32(out, hitX); write_f32(out, hitY); write_f32(out, hitZ);
    write_f32(out, normalX); write_f32(out, normalY); write_f32(out, normalZ);
    write_f32(out, damageApplied);
    write_u16(out, newAmmoCount);
    write_u32(out, serverSeed);
    return out;
}

std::optional<ShootResult> ShootResult::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ShootResult)) return std::nullopt;

    ShootResult r;
    uint32_t tmp = 0;
    if (!read_u32(buf, off, r.clientShotId)) return std::nullopt;
    if (!read_u32(buf, off, r.serverTick)) return std::nullopt;
    if (!read_u8(buf, off, r.accepted)) return std::nullopt;
    if (!read_u8(buf, off, r.didHit)) return std::nullopt;
    if (!read_u32(buf, off, tmp)) return std::nullopt;
    r.hitEntityId = static_cast<int32_t>(tmp);
    if (!read_f32(buf, off, r.hitX)) return std::nullopt;
    if (!read_f32(buf, off, r.hitY)) return std::nullopt;
    if (!read_f32(buf, off, r.hitZ)) return std::nullopt;
    if (!read_f32(buf, off, r.normalX)) return std::nullopt;
    if (!read_f32(buf, off, r.normalY)) return std::nullopt;
    if (!read_f32(buf, off, r.normalZ)) return std::nullopt;
    if (!read_f32(buf, off, r.damageApplied)) return std::nullopt;
    if (!read_u16(buf, off, r.newAmmoCount)) return std::nullopt;
    if (!read_u32(buf, off, r.serverSeed)) return std::nullopt;
    return r;
}

// -------------------- PlayerPosition --------------------
std::vector<uint8_t> PlayerPosition::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 6 * 4);
    write_u8(out, static_cast<uint8_t>(PacketType::PlayerPosition));
    write_u32(out, sequenceNumber);
    write_f32(out, posX); write_f32(out, posY); write_f32(out, posZ);
    write_f32(out, velX); write_f32(out, velY); write_f32(out, velZ);
    return out;
}

std::optional<PlayerPosition> PlayerPosition::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::PlayerPosition)) return std::nullopt;

    PlayerPosition p;
    if (!read_u32(buf, off, p.sequenceNumber)) return std::nullopt;
    if (!read_f32(buf, off, p.posX)) return std::nullopt;
    if (!read_f32(buf, off, p.posY)) return std::nullopt;
    if (!read_f32(buf, off, p.posZ)) return std::nullopt;
    if (!read_f32(buf, off, p.velX)) return std::nullopt;
    if (!read_f32(buf, off, p.velY)) return std::nullopt;
    if (!read_f32(buf, off, p.velZ)) return std::nullopt;
    return p;
}

// -------------------- PlayerSnapshotFrame --------------------
std::vector<uint8_t> PlayerSnapshotFrame::serialize() const {
    std::vector<uint8_t> out;
    constexpr size_t kEntrySize = 8 + (8 * 4) + 3 + 2 + 4 + 1 + 4 + 1 + 4 + 4;
    out.reserve(1 + 4 + 8 + 4 + 4 + players.size() * kEntrySize);
    write_u8(out, static_cast<uint8_t>(PacketType::PlayerSnapshot));
    write_u32(out, serverTick);
    write_u64(out, selfPlayerId);
    write_u32(out, lastProcessedInputTick);
    write_u32(out, static_cast<uint32_t>(players.size()));
    for (const PlayerSnapshot& p : players) {
        write_u64(out, p.id);
        write_f32(out, p.px); write_f32(out, p.py); write_f32(out, p.pz);
        write_f32(out, p.vx); write_f32(out, p.vy); write_f32(out, p.vz);
        write_f32(out, p.yaw); write_f32(out, p.pitch);
        write_u8(out, p.onGround);
        write_u8(out, p.flyMode);
        write_u8(out, p.allowFlyMode);
        write_u16(out, p.weaponId);
        write_f32(out, p.health);
        write_u8(out, p.isAlive);
        write_f32(out, p.respawnSeconds);
        write_u8(out, p.jumpPressedLastTick);
        write_f32(out, p.timeSinceGrounded);
        write_f32(out, p.jumpBufferTimer);
    }
    return out;
}

std::optional<PlayerSnapshotFrame> PlayerSnapshotFrame::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint32_t count = 0;
    constexpr size_t kEntrySize = 8 + (8 * 4) + 3 + 2 + 4 + 1 + 4 + 1 + 4 + 4;

    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::PlayerSnapshot)) return std::nullopt;

    PlayerSnapshotFrame frame;
    if (!read_u32(buf, off, frame.serverTick)) return std::nullopt;
    if (!read_u64(buf, off, frame.selfPlayerId)) return std::nullopt;
    if (!read_u32(buf, off, frame.lastProcessedInputTick)) return std::nullopt;
    if (!read_u32(buf, off, count)) return std::nullopt;
    if (count > ((buf.size() - off) / kEntrySize)) return std::nullopt;

    frame.players.clear();
    frame.players.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PlayerSnapshot p{};
        if (!read_u64(buf, off, p.id)) return std::nullopt;
        if (!read_f32(buf, off, p.px)) return std::nullopt;
        if (!read_f32(buf, off, p.py)) return std::nullopt;
        if (!read_f32(buf, off, p.pz)) return std::nullopt;
        if (!read_f32(buf, off, p.vx)) return std::nullopt;
        if (!read_f32(buf, off, p.vy)) return std::nullopt;
        if (!read_f32(buf, off, p.vz)) return std::nullopt;
        if (!read_f32(buf, off, p.yaw)) return std::nullopt;
        if (!read_f32(buf, off, p.pitch)) return std::nullopt;
        if (!read_u8(buf, off, p.onGround)) return std::nullopt;
        if (!read_u8(buf, off, p.flyMode)) return std::nullopt;
        if (!read_u8(buf, off, p.allowFlyMode)) return std::nullopt;
        if (!read_u16(buf, off, p.weaponId)) return std::nullopt;
        if (!read_f32(buf, off, p.health)) return std::nullopt;
        if (!read_u8(buf, off, p.isAlive)) return std::nullopt;
        if (!read_f32(buf, off, p.respawnSeconds)) return std::nullopt;
        if (!read_u8(buf, off, p.jumpPressedLastTick)) return std::nullopt;
        if (!read_f32(buf, off, p.timeSinceGrounded)) return std::nullopt;
        if (!read_f32(buf, off, p.jumpBufferTimer)) return std::nullopt;
        frame.players.push_back(p);
    }
    return frame;
}

// -------------------- ChunkRequest --------------------
std::vector<uint8_t> ChunkRequest::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 4 + 2);
    write_u8(out, static_cast<uint8_t>(PacketType::ChunkRequest));
    write_i32(out, chunkX);
    write_i32(out, chunkY);
    write_i32(out, chunkZ);
    write_u16(out, viewDistance);
    return out;
}

std::optional<ChunkRequest> ChunkRequest::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ChunkRequest)) return std::nullopt;

    ChunkRequest req;
    if (!read_i32(buf, off, req.chunkX)) return std::nullopt;
    if (!read_i32(buf, off, req.chunkY)) return std::nullopt;
    if (!read_i32(buf, off, req.chunkZ)) return std::nullopt;
    if (!read_u16(buf, off, req.viewDistance)) return std::nullopt;
    return req;
}

// -------------------- ChunkData --------------------
std::vector<uint8_t> ChunkData::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 4 + 8 + 1 + 4 + payload.size());
    write_u8(out, static_cast<uint8_t>(PacketType::ChunkData));
    write_i32(out, chunkX);
    write_i32(out, chunkY);
    write_i32(out, chunkZ);
    write_u64(out, version);
    write_u8(out, flags);
    write_u32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<ChunkData> ChunkData::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint32_t payloadSize = 0;

    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ChunkData)) return std::nullopt;

    ChunkData d;
    if (!read_i32(buf, off, d.chunkX)) return std::nullopt;
    if (!read_i32(buf, off, d.chunkY)) return std::nullopt;
    if (!read_i32(buf, off, d.chunkZ)) return std::nullopt;
    if (!read_u64(buf, off, d.version)) return std::nullopt;
    if (!read_u8(buf, off, d.flags)) return std::nullopt;
    if (!read_u32(buf, off, payloadSize)) return std::nullopt;
    if (off + payloadSize > buf.size()) return std::nullopt;

    d.payload.resize(payloadSize);
    if (payloadSize > 0) {
        std::memcpy(d.payload.data(), buf.data() + off, payloadSize);
    }
    return d;
}

// -------------------- ChunkDelta --------------------
std::vector<uint8_t> ChunkDelta::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 4 + 8 + 4 + edits.size() * 4);
    write_u8(out, static_cast<uint8_t>(PacketType::ChunkDelta));
    write_i32(out, chunkX);
    write_i32(out, chunkY);
    write_i32(out, chunkZ);
    write_u64(out, resultingVersion);
    write_u32(out, static_cast<uint32_t>(edits.size()));
    for (const ChunkDeltaOp& op : edits) {
        write_u8(out, op.x);
        write_u8(out, op.y);
        write_u8(out, op.z);
        write_u8(out, op.blockId);
    }
    return out;
}

std::optional<ChunkDelta> ChunkDelta::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint32_t count = 0;

    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ChunkDelta)) return std::nullopt;

    ChunkDelta d;
    if (!read_i32(buf, off, d.chunkX)) return std::nullopt;
    if (!read_i32(buf, off, d.chunkY)) return std::nullopt;
    if (!read_i32(buf, off, d.chunkZ)) return std::nullopt;
    if (!read_u64(buf, off, d.resultingVersion)) return std::nullopt;
    if (!read_u32(buf, off, count)) return std::nullopt;

    d.edits.clear();
    d.edits.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ChunkDeltaOp op;
        if (!read_u8(buf, off, op.x)) return std::nullopt;
        if (!read_u8(buf, off, op.y)) return std::nullopt;
        if (!read_u8(buf, off, op.z)) return std::nullopt;
        if (!read_u8(buf, off, op.blockId)) return std::nullopt;
        d.edits.push_back(op);
    }
    return d;
}

// -------------------- ChunkUnload --------------------
std::vector<uint8_t> ChunkUnload::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 4);
    write_u8(out, static_cast<uint8_t>(PacketType::ChunkUnload));
    write_i32(out, chunkX);
    write_i32(out, chunkY);
    write_i32(out, chunkZ);
    return out;
}

std::optional<ChunkUnload> ChunkUnload::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ChunkUnload)) return std::nullopt;

    ChunkUnload p;
    if (!read_i32(buf, off, p.chunkX)) return std::nullopt;
    if (!read_i32(buf, off, p.chunkY)) return std::nullopt;
    if (!read_i32(buf, off, p.chunkZ)) return std::nullopt;
    return p;
}

// -------------------- InventoryActionRequest --------------------
std::vector<uint8_t> InventoryActionRequest::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4 + 1 + 2 + 2 + 2);
    write_u8(out, static_cast<uint8_t>(PacketType::InventoryActionRequest));
    write_u32(out, requestId);
    write_u32(out, expectedRevision);
    write_u8(out, static_cast<uint8_t>(action.type));
    write_u16(out, action.sourceSlot);
    write_u16(out, action.destinationSlot);
    write_u16(out, action.amount);
    return out;
}

std::optional<InventoryActionRequest> InventoryActionRequest::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint8_t actionTypeRaw = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::InventoryActionRequest)) return std::nullopt;

    InventoryActionRequest req{};
    if (!read_u32(buf, off, req.requestId)) return std::nullopt;
    if (!read_u32(buf, off, req.expectedRevision)) return std::nullopt;
    if (!read_u8(buf, off, actionTypeRaw)) return std::nullopt;
    if (!read_u16(buf, off, req.action.sourceSlot)) return std::nullopt;
    if (!read_u16(buf, off, req.action.destinationSlot)) return std::nullopt;
    if (!read_u16(buf, off, req.action.amount)) return std::nullopt;
    if (off != buf.size()) return std::nullopt;
    if (actionTypeRaw > static_cast<uint8_t>(InventoryActionType::Use)) return std::nullopt;
    req.action.type = static_cast<InventoryActionType>(actionTypeRaw);
    return req;
}

// -------------------- InventoryActionResult --------------------
std::vector<uint8_t> InventoryActionResult::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 1 + 1 + 4 + 2 + (changedSlots.size() * 2));
    write_u8(out, static_cast<uint8_t>(PacketType::InventoryActionResult));
    write_u32(out, requestId);
    write_u8(out, accepted ? 1u : 0u);
    write_u8(out, static_cast<uint8_t>(rejectReason));
    write_u32(out, newRevision);
    write_u16(out, static_cast<uint16_t>(changedSlots.size()));
    for (uint16_t slotIndex : changedSlots) {
        write_u16(out, slotIndex);
    }
    return out;
}

std::optional<InventoryActionResult> InventoryActionResult::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint8_t acceptedRaw = 0;
    uint8_t rejectRaw = 0;
    uint16_t changedCount = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::InventoryActionResult)) return std::nullopt;

    InventoryActionResult result{};
    if (!read_u32(buf, off, result.requestId)) return std::nullopt;
    if (!read_u8(buf, off, acceptedRaw)) return std::nullopt;
    if (!read_u8(buf, off, rejectRaw)) return std::nullopt;
    if (!read_u32(buf, off, result.newRevision)) return std::nullopt;
    if (!read_u16(buf, off, changedCount)) return std::nullopt;
    if (changedCount > ((buf.size() - off) / 2)) return std::nullopt;

    if (rejectRaw > static_cast<uint8_t>(InventoryRejectReason::NotUsable)) return std::nullopt;
    result.accepted = (acceptedRaw != 0) ? 1u : 0u;
    result.rejectReason = static_cast<InventoryRejectReason>(rejectRaw);
    result.changedSlots.clear();
    result.changedSlots.reserve(changedCount);
    for (uint16_t i = 0; i < changedCount; ++i) {
        uint16_t slotIndex = 0;
        if (!read_u16(buf, off, slotIndex)) return std::nullopt;
        result.changedSlots.push_back(slotIndex);
    }
    if (off != buf.size()) return std::nullopt;
    return result;
}

// -------------------- InventorySnapshot --------------------
std::vector<uint8_t> InventorySnapshot::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 2 + (slots.size() * 4));
    write_u8(out, static_cast<uint8_t>(PacketType::InventorySnapshot));
    write_u32(out, revision);
    write_u16(out, static_cast<uint16_t>(slots.size()));
    for (const Slot& slot : slots) {
        write_u16(out, slot.itemId);
        write_u16(out, slot.quantity);
    }
    return out;
}

std::optional<InventorySnapshot> InventorySnapshot::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint16_t slotCount = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::InventorySnapshot)) return std::nullopt;

    InventorySnapshot snapshot{};
    if (!read_u32(buf, off, snapshot.revision)) return std::nullopt;
    if (!read_u16(buf, off, slotCount)) return std::nullopt;
    if (slotCount > ((buf.size() - off) / 4)) return std::nullopt;
    if (slotCount != kInventorySlotCount) return std::nullopt;

    snapshot.slots.clear();
    snapshot.slots.reserve(slotCount);
    for (uint16_t i = 0; i < slotCount; ++i) {
        Slot slot{};
        if (!read_u16(buf, off, slot.itemId)) return std::nullopt;
        if (!read_u16(buf, off, slot.quantity)) return std::nullopt;
        snapshot.slots.push_back(slot);
    }
    if (off != buf.size()) return std::nullopt;
    return snapshot;
}

// -------------------- WorldItemSnapshot --------------------
std::vector<uint8_t> WorldItemSnapshot::serialize() const {
    std::vector<uint8_t> out;
    constexpr size_t kEntrySize = 8 + 2 + 2 + (6 * 4);
    out.reserve(1 + 4 + 2 + (items.size() * kEntrySize));
    write_u8(out, static_cast<uint8_t>(PacketType::WorldItemSnapshot));
    write_u32(out, serverTick);
    write_u16(out, static_cast<uint16_t>(items.size()));
    for (const WorldItemState& item : items) {
        write_u64(out, item.id);
        write_u16(out, item.itemId);
        write_u16(out, item.quantity);
        write_f32(out, item.px);
        write_f32(out, item.py);
        write_f32(out, item.pz);
        write_f32(out, item.vx);
        write_f32(out, item.vy);
        write_f32(out, item.vz);
    }
    return out;
}

std::optional<WorldItemSnapshot> WorldItemSnapshot::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    uint16_t itemCount = 0;
    constexpr size_t kEntrySize = 8 + 2 + 2 + (6 * 4);
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::WorldItemSnapshot)) return std::nullopt;

    WorldItemSnapshot snapshot{};
    if (!read_u32(buf, off, snapshot.serverTick)) return std::nullopt;
    if (!read_u16(buf, off, itemCount)) return std::nullopt;
    if (itemCount > ((buf.size() - off) / kEntrySize)) return std::nullopt;

    snapshot.items.clear();
    snapshot.items.reserve(itemCount);
    for (uint16_t i = 0; i < itemCount; ++i) {
        WorldItemState item{};
        if (!read_u64(buf, off, item.id)) return std::nullopt;
        if (!read_u16(buf, off, item.itemId)) return std::nullopt;
        if (!read_u16(buf, off, item.quantity)) return std::nullopt;
        if (!read_f32(buf, off, item.px)) return std::nullopt;
        if (!read_f32(buf, off, item.py)) return std::nullopt;
        if (!read_f32(buf, off, item.pz)) return std::nullopt;
        if (!read_f32(buf, off, item.vx)) return std::nullopt;
        if (!read_f32(buf, off, item.vy)) return std::nullopt;
        if (!read_f32(buf, off, item.vz)) return std::nullopt;
        snapshot.items.push_back(item);
    }
    if (off != buf.size()) return std::nullopt;
    return snapshot;
}

