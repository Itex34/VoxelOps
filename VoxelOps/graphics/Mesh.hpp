#pragma once

#include <cstddef>
#include <cstdint>

struct VertexPacked {
    uint16_t px, py, pz;
    uint32_t normal;
    uint16_t u, v;
    uint32_t color;
};

struct VoxelVertex {
    uint32_t low;
    uint32_t high;
};

struct BufferRange {
    size_t offset = 0;
    size_t count = 0;
};

enum class ChunkMeshStatus { Ok, OutOfMemory };

struct ChunkMesh {
    BufferRange vertexRange;
    BufferRange indexRange;
    uint32_t indexCount = 0;
    bool valid = false;
    ChunkMeshStatus status = ChunkMeshStatus::Ok;
};
