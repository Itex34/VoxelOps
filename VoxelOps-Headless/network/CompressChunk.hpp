#pragma once

#include <cstdint>
#include <vector>

struct CompressedChunkPayload {
    std::vector<uint8_t> payload;
    bool compressed = false;
};

// Compresses a serialized chunk payload for network transport.
// When compressed == true, payload layout is:
//   [rawSize:u32 little-endian][lz4 block bytes]
CompressedChunkPayload CompressChunkPayload(const std::vector<uint8_t>& rawPayload);
