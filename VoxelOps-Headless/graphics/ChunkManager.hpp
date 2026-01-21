#pragma once

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <mutex>
#include <optional>
#include <cstdint>
#include <cmath>
#include <memory>

#include "../voxels/ServerChunk.hpp"
#include "../ExternLibs/FastNoiseLite.h"

// world extents in chunk coordinates (keep in sync with your constants elsewhere)
constexpr int WORLD_MIN_X = -20;
constexpr int WORLD_MAX_X = 20;
constexpr int WORLD_MIN_Z = -20;
constexpr int WORLD_MAX_Z = 20;
constexpr int WORLD_MIN_Y = -16;
constexpr int WORLD_MAX_Y = 32;

struct IVec3Hash {
    std::size_t operator()(glm::ivec3 const& v) const noexcept {
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t z = static_cast<uint32_t>(v.z);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
        return static_cast<std::size_t>(h);
    }
};
struct IVec3Eq {
    bool operator()(glm::ivec3 const& a, glm::ivec3 const& b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

class ChunkManager {
public:
    ChunkManager(uint64_t seed = 1337u);
    ~ChunkManager() = default;

    // Non-copyable
    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;

    // Generation / initialization
    void generateInitialChunks(int radiusChunks);
    void generateInitialChunks_TwoPass(int radiusChunks);

    // Per-chunk generation helpers
    // generateChunkAt will synchronously generate and insert the chunk if in-bounds.
    void generateChunkAt(const glm::ivec3& pos);
    void generateTerrainChunkAt(const glm::ivec3& pos);

    // Chunk lifecycle / updates
    void updateDirtyChunks();
    void updateChunks(const glm::ivec3& playerWorldPos, int renderDistance);

    // Block access (thread-safe wrappers that find the chunk then call chunk methods)
    void setBlockInWorld(const glm::ivec3& worldPos, BlockID blockID);
    void setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id);
    BlockID getBlockGlobal(int worldX, int worldY, int worldZ);

    // Safe local block access (handles cross-chunk writes)
    void setBlockSafe(ServerChunk& currentChunk, const glm::ivec3& pos, BlockID id);
    BlockID getBlockSafe(ServerChunk& currentChunk, const glm::ivec3& pos);

    // Utilities
    glm::ivec3 worldToChunkPos(const glm::ivec3& worldPos) const;
    glm::ivec3 worldToLocalPos(const glm::ivec3& worldPos) const;
    bool inBounds(const glm::ivec3& pos) const;

    void markChunkDirty(const glm::ivec3& pos);
    std::array<bool, 6> getVisibleChunkFaces(const glm::ivec3& pos) const;

    // Thread-safe access to the chunk map
    // snapshotChunkMap: returns a copy of map entries -> raw pointers (safe for iteration; chunks still protected internally)
    std::unordered_map<glm::ivec3, ServerChunk*, IVec3Hash, IVec3Eq> snapshotChunkMap() const;

    // Unsafe low-level access: returns reference to internal map. Caller MUST hold mapMutex for the duration of use.
    std::unordered_map<glm::ivec3, std::unique_ptr<ServerChunk>, IVec3Hash, IVec3Eq>& getChunksRefUnsafe() { return chunkMap; }

    // Helper: safely obtain a raw pointer to a chunk (or nullptr if missing). Snapshot-style (map lock held briefly).
    ServerChunk* getChunkIfExists(const glm::ivec3& chunkPos) const;

    // Synchronous helper: find chunk or generate it (generates off-map and inserts under lock).
    // Returns pointer to chunk in the map (never nullptr if pos in-bounds and generation succeeded).
    // Note: generation may be expensive — consider calling generateChunkAt asynchronously instead.
    ServerChunk* loadOrGenerateChunk(const glm::ivec3& chunkPos);

    // World settings / toggles
    bool enableAO = false;
    bool enableShadows = false;

private:
    // decoration helper
    void placeTree(ServerChunk& chunk, const glm::ivec3& basePos, std::mt19937& gen);

    FastNoiseLite noise;
    uint64_t worldSeed = 1337u;

    // protects chunkMap structure (only)
    mutable std::mutex mapMutex;
    std::unordered_map<glm::ivec3, std::unique_ptr<ServerChunk>, IVec3Hash, IVec3Eq> chunkMap;

    static inline int floorDiv(int a, int b) {
        int q = a / b;
        int r = a % b;
        if ((r != 0) && ((r > 0) != (b > 0))) q--;
        return q;
    }
    static inline int mod(int a, int b) {
        int r = a % b;
        if (r < 0) r += std::abs(b);
        return r;
    }
};
