#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Shared::ChunkWireFormat {

constexpr uint8_t kChunkFlagCompressed = 0x1u;
constexpr uint8_t kKnownChunkFlagsMask = kChunkFlagCompressed;
constexpr size_t kCompressedHeaderSize = sizeof(uint32_t);

inline void WriteU32LE(std::vector<uint8_t> &dst, uint32_t value) {
    dst.push_back(static_cast<uint8_t>(value & 0xFFu));
    dst.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

inline bool ReadU32LE(const std::vector<uint8_t> &src, size_t offset, uint32_t &outValue) {
    if (offset + sizeof(uint32_t) > src.size()) {
        return false;
    }
    outValue = static_cast<uint32_t>(src[offset]) | (static_cast<uint32_t>(src[offset + 1]) << 8) |
               (static_cast<uint32_t>(src[offset + 2]) << 16) |
               (static_cast<uint32_t>(src[offset + 3]) << 24);
    return true;
}

} // namespace Shared::ChunkWireFormat
