#include "DecompressChunk.hpp"

#include "../../Shared/network/ChunkWireFormat.hpp"

#include <lz4.h>

#include "../voxels/Chunk.hpp"

#include <limits>

namespace {
    constexpr uint32_t kExpectedDecodedPayloadBytes =
        static_cast<uint32_t>(CHUNK_VOLUME * sizeof(BlockID));
} // namespace

bool DecompressChunkPayload(
    uint8_t flags, const std::vector<uint8_t> &payload, std::vector<uint8_t> &outRawPayload
) {
    if ((flags & ~Shared::ChunkWireFormat::kKnownChunkFlagsMask) != 0u) {
        return false;
    }

    const bool compressed = (flags & Shared::ChunkWireFormat::kChunkFlagCompressed) != 0;
    if (!compressed) {
        if (payload.size() != static_cast<size_t>(kExpectedDecodedPayloadBytes)) {
            return false;
        }
        outRawPayload = payload;
        return true;
    }

    uint32_t rawSize = 0;
    if (!Shared::ChunkWireFormat::ReadU32LE(payload, 0, rawSize)) {
        return false;
    }
    if (rawSize != kExpectedDecodedPayloadBytes ||
        rawSize > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (payload.size() < Shared::ChunkWireFormat::kCompressedHeaderSize) {
        return false;
    }

    const size_t compressedSize = payload.size() - Shared::ChunkWireFormat::kCompressedHeaderSize;
    if (compressedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    outRawPayload.resize(static_cast<size_t>(rawSize));
    if (rawSize == 0) {
        return compressedSize == 0;
    }

    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char *>(
            payload.data() + Shared::ChunkWireFormat::kCompressedHeaderSize
        ),
        reinterpret_cast<char *>(outRawPayload.data()),
        static_cast<int>(compressedSize),
        static_cast<int>(rawSize)
    );
    return decoded == static_cast<int>(rawSize);
}
