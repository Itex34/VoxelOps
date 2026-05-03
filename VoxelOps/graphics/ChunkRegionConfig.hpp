#pragma once

#include <cstddef>

// Region size in chunks (e.g., 8x8x8 chunks per region).
constexpr int REGION_SIZE = 8;

// Bytes per region.
constexpr size_t REGION_VERTEX_BYTES = 3 * 1024 * 1024;
constexpr size_t REGION_INDEX_BYTES = 2 * 1024 * 1024;
