#pragma once

#include "../../world/ChunkManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

class RegionMeshBuffer;

class OpenGLChunkRenderCache {
  public:
    struct RegionChunkMesh {
        ChunkMesh mesh;
        uint64_t revision = 0;
    };

    struct Region {
        glm::ivec3 regionPos{0};
        std::unique_ptr<RegionMeshBuffer> gpu;
        std::unordered_map<glm::ivec3, RegionChunkMesh, IVec3Hash> chunks;
        size_t vertexBytes = REGION_VERTEX_BYTES;
        size_t indexBytes = REGION_INDEX_BYTES;

        Region() = default;
        Region(glm::ivec3 pos, size_t vertexBytes_, size_t indexBytes_);
    };

    using RegionMap = std::unordered_map<glm::ivec3, Region, IVec3Hash>;

    void syncFromChunkManager(const ChunkManager &chunkManager);
    [[nodiscard]] const RegionMap &regions() const noexcept {
        return m_regions;
    }
    [[nodiscard]] RegionMap &regions() noexcept {
        return m_regions;
    }

  private:
    Region &getOrCreateRegion(const glm::ivec3 &chunkPos);
    bool rebuildRegion(const glm::ivec3 &regionPos,
                       const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes,
                       size_t reserveVertices = 0, size_t reserveIndices = 0);
    void pruneMissingMeshes(
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes);

    static glm::ivec3 chunkToRegionPos(const glm::ivec3 &chunkPos);
    static int floorDiv(int a, int b);

    RegionMap m_regions;
};
