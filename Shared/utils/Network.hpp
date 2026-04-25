#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace Shared::Utils {


    inline void appendU8(std::vector<uint8_t>& out, uint8_t v) {
        out.push_back(v);
    }

    inline void appendU16(std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    }

    inline void appendU32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    }

    inline void appendU64(std::vector<uint8_t>& out, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    }

    inline void appendF32(std::vector<uint8_t>& out, float value) {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(out, bits);
    }

    inline bool IsNewerU32(uint32_t a, uint32_t b) {
        return static_cast<int32_t>(a - b) > 0;
    }
}
