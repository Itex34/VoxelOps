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



