#pragma once

#include "VoxelVertex.hpp"

#include <cstdint>
#include <vector>

struct CpuChunkMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<uint16_t> indices;
    uint64_t revision = 0;
};
