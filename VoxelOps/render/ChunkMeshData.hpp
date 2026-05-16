#pragma once

#include "VoxelVertex.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

struct CpuChunkMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<glm::vec3> rtVertices;
    std::vector<uint16_t> indices;
    uint64_t revision = 0;
    bool highPriorityRtBuild = false;
};
