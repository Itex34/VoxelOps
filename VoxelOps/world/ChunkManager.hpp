#pragma once

#include <glm/glm.hpp>
#include "../voxels/Chunk.hpp"
#include "../voxels/ChunkColumn.hpp"
#include "../voxels/VoxelCoordHash.hpp"

#include "../render/ChunkMeshData.hpp"

#include "../../Shared/network/Packets.hpp"
#include "../../Shared/world/Constants.hpp"

#include <optional>
#include <memory>
#include <random>
#include <unordered_map>
#include <cstdint>

//--IN BLOCKS--
constexpr int WORLD_SIZE_Y = (WORLD_MAX_Y - WORLD_MIN_Y + 1);

struct ChunkRange {
    uint32_t firstIndex;     // index offset in bigEBO (in indices)
    uint32_t indexCount;     // number of indices
    uint32_t baseVertex;     // base vertex index in bigVBO (in vertices)
    uint32_t vertexCount;    // current vertex count
    uint32_t vertexCapacity; // optional: reserved capacity for in-place updates
    uint32_t indexCapacity;  // optional: reserved capacity for in-place updates
    glm::ivec3 chunkPos;     // world chunk coordinates
    bool alive = true;
};

class ChunkMesher;

enum class NetworkChunkDeltaApplyResult {
    Applied = 0,
    MissingBaseChunk = 1,
    StaleVersion = 2,
    VersionGap = 3
};

class ChunkManager {
public:
    ChunkManager();

    ~ChunkManager();

    void setBlockInWorld(const glm::ivec3 &worldPos, BlockID blockID);

    void setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id);
    BlockID getBlockGlobal(int worldX, int worldY, int worldZ);

    void updateDirtyChunks(size_t maxChunksPerCall = 0, int64_t maxBudgetUs = 0);
    void updateDirtyChunkAt(const glm::ivec3 &chunkPos);
    void markChunkDirty(const glm::ivec3 &pos);
    void markChunkDirtyHighPriority(const glm::ivec3 &pos);

    void playerPlaceBlockAt(glm::ivec3 blockCoords, int faceNormal, BlockID blockType);
    void playerBreakBlockAt(const glm::ivec3 &blockCoords);
    bool applyNetworkChunkData(const ChunkData &packet);
    NetworkChunkDeltaApplyResult applyNetworkChunkDelta(const ChunkDelta &packet);
    void applyNetworkChunkUnload(const ChunkUnload &packet);

    const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &getChunks() const {
        return chunkMap;
    }

    std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &getChunks() {
        return chunkMap;
    }

    [[nodiscard]] bool hasChunkLoaded(const glm::ivec3 &chunkPos) const;
    [[nodiscard]] const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &
    getCpuChunkMeshes() const noexcept {
        return getCpuChunkMeshesImpl();
    }

    glm::ivec3 worldToChunkPos(const glm::ivec3 &worldPos) const;
    glm::ivec3 worldToLocalPos(const glm::ivec3 &worldPos) const;

    bool enableAO;
    void setMeshBakedLightingEnabled(bool enabled) {
        m_meshBakedLightingEnabled = enabled;
    }

    void debugMemoryEstimate();

    bool inBounds(const glm::ivec3 &pos) const;

private:
    friend class ChunkMesher;

    std::unordered_map<glm::ivec3, Chunk, IVec3Hash> chunkMap;
    std::unordered_map<glm::ivec3, uint64_t, IVec3Hash> m_networkChunkVersions;
    std::unique_ptr<ChunkMesher> m_chunkMesher;

    std::unordered_map<glm::ivec2, ChunkColumn, IVec2Hash> chunkColumns;

    void setBlockSafe(Chunk &currentChunk, const glm::ivec3 &pos, BlockID id);
    BlockID getBlockSafe(Chunk &currentChunk, const glm::ivec3 &pos);
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &
    getCpuChunkMeshesImpl() const noexcept;

    inline std::array<bool, 6> isEdgeBlock(glm::ivec3 localPos) {
        return {
            (localPos.x == 0),
            (localPos.x == CHUNK_SIZE - 1),
            (localPos.y == 0),
            (localPos.y == CHUNK_SIZE - 1),
            (localPos.z == 0),
            (localPos.z == CHUNK_SIZE - 1)
        };
    }

    inline std::array<bool, 8> isCornerBlock(glm::ivec3 localPos) {
        return {
            (localPos.x == 0 && localPos.y == 0),
            (localPos.x == (CHUNK_SIZE - 1) && localPos.y == (CHUNK_SIZE - 1)),
            (localPos.y == 0 && localPos.z == 0),
            (localPos.y == (CHUNK_SIZE - 1) && localPos.z == (CHUNK_SIZE - 1)),

            (localPos.x == 0 && localPos.y == (CHUNK_SIZE - 1)),
            (localPos.x == (CHUNK_SIZE - 1) && localPos.y == 0),
            (localPos.y == 0 && localPos.z == (CHUNK_SIZE - 1)),
            (localPos.y == (CHUNK_SIZE - 1) && localPos.z == 0),
        };
    }

    inline int floorDiv(int a, int b) const {
        int q = a / b;
        int r = a % b;
        if ((r != 0) && ((r > 0) != (b > 0))) {
            q--;
        }
        return q;
    }

    inline int mod(int a, int b) const {
        int r = a % b;
        if (r < 0)
            r += std::abs(b);
        return r;
    }

    bool m_meshBakedLightingEnabled = true;
};
