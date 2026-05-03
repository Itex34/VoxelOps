#include "PacketReader.hpp"

#include <cstring>

namespace NetPacket {

    uint16_t ReadU16LE(const uint8_t *p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    uint32_t ReadU32LE(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    int32_t ReadI32LE(const uint8_t *p) {
        return static_cast<int32_t>(ReadU32LE(p));
    }

    float ReadF32LE(const uint8_t *p) {
        const uint32_t bits = ReadU32LE(p);
        float out = 0.0f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

} // namespace NetPacket
