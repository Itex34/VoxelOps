#pragma once

#include <cstdint>

namespace NetPacket {

    uint16_t ReadU16LE(const uint8_t *p);
    uint32_t ReadU32LE(const uint8_t *p);
    int32_t ReadI32LE(const uint8_t *p);
    float ReadF32LE(const uint8_t *p);

} // namespace NetPacket
