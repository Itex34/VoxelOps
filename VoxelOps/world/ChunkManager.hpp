#pragma once

#include <glm/glm.hpp>
#include "../voxels/Chunk.hpp"
#include "../voxels/ChunkColumn.hpp"

#include "../graphics/Mesh.hpp"
#include "../graphics/AtlasLayout.hpp"

#include "../../Shared/network/Packets.hpp"
#include "../../Shared/world/Constants.hpp"

#include <optional>
#include <memory>
#include <random>
#include <unordered_map>
#include <cstdint>



//--IN BLOCKS--
constexpr int WORLD_SIZE_Y = (WORLD_MAX_Y - WORLD_MIN_Y + 1);

// Region size in chunks (e.g., 8x8x8 chunks per region)
constexpr int REGION_SIZE = 8;

// Bytes per region (tune based on your needs)
constexpr size_t REGION_VERTEX_BYTES = 3 * 1024 * 1024; // 16 MB
constexpr size_t REGION_INDEX_BYTES = 2 * 1024 * 1024;  // 8 MB

struct Vec3Hasher {
    size_t operator()(const glm::ivec3 &v) const {
        return std::hash<int>()(v.x) ^ std::hash<int>()(v.y << 1) ^ std::hash<int>()(v.z << 2);
    }
};

struct IVec3Hash {
    std::size_t operator()(glm::ivec3 const &v) const noexcept {
        // mix the three 32-bit ints into a 64-bit value then reduce to size_t
        // using large primes for simple hashing (good enough for chunk coords)
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t z = static_cast<uint32_t>(v.z);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
        return static_cast<std::size_t>(h);
    }
};

struct IVec2Hash {
    std::size_t operator()(glm::ivec2 const &v) const noexcept {
        // mix the three 32-bit ints into a 64-bit value then reduce to size_t
        // using large primes for simple hashing (good enough for chunk coords)
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u);
        return static_cast<std::size_t>(h);
    }
};

struct IVec3Eq {
    bool operator()(glm::ivec3 const &a, glm::ivec3 const &b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

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

struct CpuChunkMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<uint16_t> indices;
    uint64_t revision = 0;
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
    void updateChunks(const glm::ivec3 &playerWorldPos, int renderDistance);
    void updateDirtyChunkAt(const glm::ivec3 &chunkPos);
    void markChunkDirty(const glm::ivec3 &pos);

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
    bool enableShadows;
    void setMeshBakedLightingEnabled(bool enabled) { m_meshBakedLightingEnabled = enabled; }

    AtlasLayout atlasLayout;

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
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &getCpuChunkMeshesImpl() const
        noexcept;


    ChunkColumn &getOrCreateColumn(int colX, int colZ);

    bool suppressSunlightAffectedRebuilds = false;

    inline std::array<bool, 6> isEdgeBlock(glm::ivec3 localPos) {
        return {(localPos.x == 0), (localPos.x == CHUNK_SIZE - 1),
                (localPos.y == 0), (localPos.y == CHUNK_SIZE - 1),
                (localPos.z == 0), (localPos.z == CHUNK_SIZE - 1)};
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
