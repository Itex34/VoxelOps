#include "DecompressChunk.hpp"

#include <lz4.h>

#include "../voxels/Chunk.hpp"

#include <limits>

namespace {
constexpr uint8_t kChunkFlagCompressed = 0x1u;
constexpr uint8_t kKnownChunkFlagsMask = kChunkFlagCompressed;
constexpr size_t kCompressedHeaderSize = sizeof(uint32_t);
constexpr uint32_t kExpectedDecodedPayloadBytes = static_cast<uint32_t>(
    4u + 4u + 4u + 8u + 1u + 4u + (CHUNK_VOLUME * sizeof(BlockID))
);

inline bool ReadU32LE(const std::vector<uint8_t>& src, size_t offset, uint32_t& outValue)
{
    if (offset + sizeof(uint32_t) > src.size()) {
        return false;
    }
    outValue = static_cast<uint32_t>(src[offset]) |
        (static_cast<uint32_t>(src[offset + 1]) << 8) |
        (static_cast<uint32_t>(src[offset + 2]) << 16) |
        (static_cast<uint32_t>(src[offset + 3]) << 24);
    return true;
}
}

bool DecompressChunkPayload(uint8_t flags, const std::vector<uint8_t>& payload, std::vector<uint8_t>& outRawPayload)
{
    if ((flags & ~kKnownChunkFlagsMask) != 0u) {
        return false;
    }

    const bool compressed = (flags & kChunkFlagCompressed) != 0;
    if (!compressed) {
        if (payload.size() > static_cast<size_t>(kExpectedDecodedPayloadBytes)) {
            return false;
        }
        outRawPayload = payload;
        return true;
    }

    uint32_t rawSize = 0;
    if (!ReadU32LE(payload, 0, rawSize)) {
        return false;
    }
    if (rawSize != kExpectedDecodedPayloadBytes || rawSize > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (payload.size() < kCompressedHeaderSize) {
        return false;
    }

    const size_t compressedSize = payload.size() - kCompressedHeaderSize;
    if (compressedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    outRawPayload.resize(static_cast<size_t>(rawSize));
    if (rawSize == 0) {
        return compressedSize == 0;
    }

    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char*>(payload.data() + kCompressedHeaderSize),
        reinterpret_cast<char*>(outRawPayload.data()),
        static_cast<int>(compressedSize),
        static_cast<int>(rawSize)
    );
    return decoded == static_cast<int>(rawSize);
}
