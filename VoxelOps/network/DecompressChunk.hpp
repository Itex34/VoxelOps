#pragma once

#include <cstdint>
#include <vector>

// Decodes a chunk payload from the network.
// If flags bit0 is unset, output is the payload as-is.
// If flags bit0 is set, payload must be:
//   [rawSize:u32 little-endian][lz4 block bytes]
bool DecompressChunkPayload(uint8_t flags, const std::vector<uint8_t>& payload, std::vector<uint8_t>& outRawPayload);
