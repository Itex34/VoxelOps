#include "Packets.hpp"
#include <cstring>
#include <cassert>

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

// -------------------- ChunkAck --------------------
std::vector<uint8_t> ChunkAck::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(1 + 1 + 4 + 4 + 4 + 4 + 8);
    write_u8(out, static_cast<uint8_t>(PacketType::ChunkAck));
    write_u8(out, ackedType);
    write_u32(out, sequence);
    write_i32(out, chunkX);
    write_i32(out, chunkY);
    write_i32(out, chunkZ);
    write_u64(out, version);
    return out;
}

std::optional<ChunkAck> ChunkAck::deserialize(const std::vector<uint8_t>& buf) {
    size_t off = 0;
    uint8_t type = 0;
    if (!read_u8(buf, off, type)) return std::nullopt;
    if (type != static_cast<uint8_t>(PacketType::ChunkAck)) return std::nullopt;

    ChunkAck ack;
    if (!read_u8(buf, off, ack.ackedType)) return std::nullopt;
    if (!read_u32(buf, off, ack.sequence)) return std::nullopt;
    if (!read_i32(buf, off, ack.chunkX)) return std::nullopt;
    if (!read_i32(buf, off, ack.chunkY)) return std::nullopt;
    if (!read_i32(buf, off, ack.chunkZ)) return std::nullopt;
    if (!read_u64(buf, off, ack.version)) return std::nullopt;
    return ack;
}



