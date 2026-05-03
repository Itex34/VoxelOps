#include "CompressChunk.hpp"

#include "../../../Shared/network/ChunkWireFormat.hpp"

#include <lz4.h>

#include <limits>

namespace {
    constexpr size_t kMinCompressBytes = 1024;
    constexpr size_t kMinSavingsBytes = 64;
    constexpr size_t kMinSavingsPercent = 8;
} // namespace

CompressedChunkPayload CompressChunkPayload(const std::vector<uint8_t> &rawPayload) {
    CompressedChunkPayload result;
    result.payload = rawPayload;
    result.compressed = false;

    if (rawPayload.size() < kMinCompressBytes) {
        return result;
    }
    if (rawPayload.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return result;
    }

    const int inputSize = static_cast<int>(rawPayload.size());
    const int bound = LZ4_compressBound(inputSize);
    if (bound <= 0) {
        return result;
    }

    std::vector<uint8_t> candidate;
    candidate.reserve(Shared::ChunkWireFormat::kCompressedHeaderSize + static_cast<size_t>(bound));
    Shared::ChunkWireFormat::WriteU32LE(candidate, static_cast<uint32_t>(rawPayload.size()));
    candidate.resize(Shared::ChunkWireFormat::kCompressedHeaderSize + static_cast<size_t>(bound));

    const int compressedSize = LZ4_compress_default(
        reinterpret_cast<const char *>(rawPayload.data()),
        reinterpret_cast<char *>(candidate.data() + Shared::ChunkWireFormat::kCompressedHeaderSize),
        inputSize,
        bound
    );
    if (compressedSize <= 0) {
        return result;
    }

    candidate.resize(
        Shared::ChunkWireFormat::kCompressedHeaderSize + static_cast<size_t>(compressedSize)
    );

    const size_t compressedTotal = candidate.size();
    const size_t requiredSavingsByPercent = (rawPayload.size() * kMinSavingsPercent) / 100;
    const size_t requiredSavings =
        (requiredSavingsByPercent > kMinSavingsBytes) ? requiredSavingsByPercent : kMinSavingsBytes;
    if (compressedTotal + requiredSavings > rawPayload.size()) {
        return result;
    }

    result.payload = std::move(candidate);
    result.compressed = true;
    return result;
}
