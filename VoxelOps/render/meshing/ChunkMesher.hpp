#pragma once

#include "../../world/ChunkManager.hpp"
#include "ChunkMeshBuilder.hpp"
#include "../../graphics/AtlasLayout.hpp"
#include "../../misc/ThreadPool.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChunkMesher {
public:
    explicit ChunkMesher(ChunkManager &owner);
    ~ChunkMesher();

    void markChunkDirty(const glm::ivec3 &pos);
    void updateDirtyChunks(size_t maxChunksPerCall = 0, int64_t maxBudgetUs = 0);
    void updateDirtyChunkAt(const glm::ivec3 &chunkPos);
    void onChunkRemoved(const glm::ivec3 &chunkPos);

    [[nodiscard]] const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &
    getCpuChunkMeshes() const noexcept {
        return m_cpuChunkMeshes;
    }

private:
    struct ChunkMeshBuildJob {
        glm::ivec3 chunkPos{0};
        uint64_t buildTicket = 0;
        bool enableAO = false;
        int chunkWorldMinX = 0;
        int chunkWorldMinZ = 0;
        std::array<BlockID, CHUNK_VOLUME> centerBlocks{};
        std::array<std::array<BlockID, CHUNK_VOLUME>, 6> neighborBlocks{};
        std::array<uint8_t, 6> neighborPresent{};
    };

    struct ChunkMeshBuildResult {
        glm::ivec3 chunkPos{0};
        uint64_t buildTicket = 0;
        std::vector<VoxelVertex> vertices;
        std::vector<uint16_t> indices;
    };

    bool requestChunkRebuild(const glm::ivec3 &pos);
    void buildChunkMeshWorker(ChunkMeshBuildJob job);

    ChunkManager &m_owner;

    std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> m_cpuChunkMeshes;
    uint64_t m_nextCpuChunkMeshRevision = 1;

    std::deque<glm::ivec3> m_dirtyChunkQueue;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> m_dirtyChunkPending;
    std::deque<ChunkMeshBuildResult> m_readyChunkMeshes;
    std::mutex m_readyChunkMeshesMutex;
    std::unordered_map<glm::ivec3, uint64_t, IVec3Hash> m_chunkBuildTickets;
    std::atomic<uint64_t> m_nextChunkBuildTicket{1};

    AtlasLayout m_atlasLayout;
    ThreadPool m_meshPool{std::max(1u, std::thread::hardware_concurrency() - 1)};
};
