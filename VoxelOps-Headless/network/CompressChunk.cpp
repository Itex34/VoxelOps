#include "CompressChunk.hpp"

#include <lz4.h>

#include <limits>

namespace {
constexpr size_t kMinCompressBytes = 1024;
constexpr size_t kMinSavingsBytes = 64;
constexpr size_t kMinSavingsPercent = 8;

inline void WriteU32LE(std::vector<uint8_t>& dst, uint32_t value)
{
    dst.push_back(static_cast<uint8_t>(value & 0xFFu));
    dst.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}
}

CompressedChunkPayload CompressChunkPayload(const std::vector<uint8_t>& rawPayload)
{
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
    candidate.reserve(sizeof(uint32_t) + static_cast<size_t>(bound));
    WriteU32LE(candidate, static_cast<uint32_t>(rawPayload.size()));
    candidate.resize(sizeof(uint32_t) + static_cast<size_t>(bound));

    const int compressedSize = LZ4_compress_default(
        reinterpret_cast<const char*>(rawPayload.data()),
        reinterpret_cast<char*>(candidate.data() + sizeof(uint32_t)),
        inputSize,
        bound
    );
    if (compressedSize <= 0) {
        return result;
    }

    candidate.resize(sizeof(uint32_t) + static_cast<size_t>(compressedSize));

    const size_t compressedTotal = candidate.size();
    const size_t requiredSavingsByPercent = (rawPayload.size() * kMinSavingsPercent) / 100;
    const size_t requiredSavings = (requiredSavingsByPercent > kMinSavingsBytes)
        ? requiredSavingsByPercent
        : kMinSavingsBytes;
    if (compressedTotal + requiredSavings > rawPayload.size()) {
        return result;
    }

    result.payload = std::move(candidate);
    result.compressed = true;
    return result;
}
