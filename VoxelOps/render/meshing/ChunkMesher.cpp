#include "ChunkMesher.hpp"

#include <iostream>

namespace {
    constexpr double kChunkMeshBuildLogThresholdMs = 2.0;
    constexpr double kChunkMeshUploadLogThresholdMs = 2.0;
} // namespace

ChunkMesher::ChunkMesher(ChunkManager &owner)
    : m_owner(owner) {
    ChunkMeshBuilder::resetProfileSnapshot();
}

ChunkMesher::~ChunkMesher() = default;

void ChunkMesher::markChunkDirty(const glm::ivec3 &pos) {
    if (!m_owner.inBounds(pos)) {
        return;
    }
    auto it = m_owner.chunkMap.find(pos);
    if (it == m_owner.chunkMap.end()) {
        return;
    }

    it->second.dirty = true;
    if (m_dirtyChunkPending.insert(pos).second) {
        m_dirtyChunkQueue.push_back(pos);
    }
}

void ChunkMesher::updateDirtyChunks(size_t maxChunksPerCall, int64_t maxBudgetUs) {
    const auto start = std::chrono::steady_clock::now();
    size_t scheduled = 0;
    const auto outOfBudget = [&]() {
        if (maxBudgetUs > 0) {
            const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start
            )
                                          .count();
            if (elapsedUs >= maxBudgetUs) {
                return true;
            }
        }
        return false;
    };

    while (!outOfBudget()) {
        ChunkMeshBuildResult ready;
        {
            std::lock_guard<std::mutex> lock(m_readyChunkMeshesMutex);
            if (m_readyChunkMeshes.empty()) {
                break;
            }
            ready = std::move(m_readyChunkMeshes.front());
            m_readyChunkMeshes.pop_front();
        }

        auto it = m_owner.chunkMap.find(ready.chunkPos);
        if (it != m_owner.chunkMap.end()) {
            const auto ticketIt = m_chunkBuildTickets.find(ready.chunkPos);
            if (ticketIt == m_chunkBuildTickets.end() || ticketIt->second != ready.buildTicket) {
                continue;
            }

            Chunk &chunk = it->second;
            chunk.building = false;

            if (!chunk.dirty.load(std::memory_order_acquire)) {
                const auto uploadStart = std::chrono::steady_clock::now();
                if (ready.vertices.empty() || ready.indices.empty()) {
                    m_cpuChunkMeshes.erase(ready.chunkPos);
                } else {
                    CpuChunkMesh &cpuMesh = m_cpuChunkMeshes[ready.chunkPos];
                    cpuMesh.vertices = std::move(ready.vertices);
                    cpuMesh.indices = std::move(ready.indices);
                    cpuMesh.revision = m_nextCpuChunkMeshRevision++;
                }
                const auto uploadEnd = std::chrono::steady_clock::now();
                const double uploadMs =
                    std::chrono::duration<double, std::milli>(uploadEnd - uploadStart).count();
                if (uploadMs >= kChunkMeshUploadLogThresholdMs) {
                    std::cerr << "[chunk/mesh] slow uploadMs=" << uploadMs
                              << " verts=" << ready.vertices.size()
                              << " idx=" << ready.indices.size() << " chunk=(" << ready.chunkPos.x
                              << "," << ready.chunkPos.y << "," << ready.chunkPos.z << ")\n";
                }
            } else if (m_dirtyChunkPending.insert(ready.chunkPos).second) {
                m_dirtyChunkQueue.push_back(ready.chunkPos);
            }
        }
    }

    while (!m_dirtyChunkQueue.empty() && !outOfBudget()) {
        if (maxChunksPerCall > 0 && scheduled >= maxChunksPerCall) {
            break;
        }

        const glm::ivec3 pos = m_dirtyChunkQueue.front();
        m_dirtyChunkQueue.pop_front();
        m_dirtyChunkPending.erase(pos);

        auto it = m_owner.chunkMap.find(pos);
        if (it == m_owner.chunkMap.end()) {
            continue;
        }
        if (!it->second.dirty.load(std::memory_order_acquire)) {
            continue;
        }

        if (!requestChunkRebuild(pos)) {
            auto chunkIt = m_owner.chunkMap.find(pos);
            if (chunkIt != m_owner.chunkMap.end() &&
                chunkIt->second.dirty.load(std::memory_order_acquire) &&
                m_dirtyChunkPending.insert(pos).second) {
                m_dirtyChunkQueue.push_back(pos);
            }
        }
        ++scheduled;
    }
}

void ChunkMesher::updateDirtyChunkAt(const glm::ivec3 &chunkPos) {
    markChunkDirty(chunkPos);
    (void)requestChunkRebuild(chunkPos);
}

void ChunkMesher::onChunkRemoved(const glm::ivec3 &chunkPos) {
    m_chunkBuildTickets.erase(chunkPos);
    m_cpuChunkMeshes.erase(chunkPos);
    m_dirtyChunkPending.erase(chunkPos);
}

bool ChunkMesher::requestChunkRebuild(const glm::ivec3 &pos) {
    auto it = m_owner.chunkMap.find(pos);
    if (it == m_owner.chunkMap.end()) {
        return false;
    }

    Chunk &chunk = it->second;
    bool expected = false;
    if (!chunk.building.compare_exchange_strong(expected, true)) {
        return false;
    }

    chunk.dirty = false;

    ChunkMeshBuildJob job;
    job.chunkPos = pos;
    job.buildTicket = m_nextChunkBuildTicket.fetch_add(1, std::memory_order_relaxed);
    m_chunkBuildTickets[pos] = job.buildTicket;
    // Vulkan path uses shader-side GI/lighting, so keep chunk meshing to pure greedy geometry.
    const bool meshBakedLightingEnabled = m_owner.m_meshBakedLightingEnabled;
    job.enableAO = meshBakedLightingEnabled && m_owner.enableAO;
    job.chunkWorldMinX = pos.x * CHUNK_SIZE;
    job.chunkWorldMinZ = pos.z * CHUNK_SIZE;
    chunk.copyBlocks(job.centerBlocks);

    constexpr glm::ivec3 offsets[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };

    for (int i = 0; i < 6; ++i) {
        auto neighborIt = m_owner.chunkMap.find(pos + offsets[i]);
        if (neighborIt == m_owner.chunkMap.end()) {
            continue;
        }
        job.neighborPresent[static_cast<size_t>(i)] = 1;
        neighborIt->second.copyBlocks(job.neighborBlocks[static_cast<size_t>(i)]);
    }

    m_meshPool.enqueue([this, job = std::move(job)]() mutable {
        this->buildChunkMeshWorker(std::move(job));
    });
    return true;
}

void ChunkMesher::buildChunkMeshWorker(ChunkMeshBuildJob job) {
    thread_local ChunkMeshBuilder workerBuilder;

    Chunk center(job.chunkPos);
    center.overwriteBlocks(job.centerBlocks);

    constexpr glm::ivec3 offsets[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };

    std::array<std::optional<Chunk>, 6> neighborStorage;
    const Chunk *neighbors[6] = {};
    for (int i = 0; i < 6; ++i) {
        if (job.neighborPresent[static_cast<size_t>(i)] == 0) {
            continue;
        }

        neighborStorage[static_cast<size_t>(i)].emplace(job.chunkPos + offsets[i]);
        neighborStorage[static_cast<size_t>(i)]->overwriteBlocks(
            job.neighborBlocks[static_cast<size_t>(i)]
        );
        neighbors[i] = &neighborStorage[static_cast<size_t>(i)].value();
    }

    const auto buildStart = std::chrono::steady_clock::now();
    auto built = workerBuilder.buildChunkMesh(center, neighbors, job.chunkPos, m_atlasLayout, job.enableAO);

    const auto buildEnd = std::chrono::steady_clock::now();
    const double buildMs = std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();
    if (buildMs >= kChunkMeshBuildLogThresholdMs) {
        std::cerr << "[chunk/mesh] slow workerBuildMs=" << buildMs
                  << " verts=" << built.vertices.size() << " idx=" << built.indices.size()
                  << " chunk=(" << job.chunkPos.x << "," << job.chunkPos.y << "," << job.chunkPos.z
                  << ")\n";
    }

    ChunkMeshBuildResult ready;
    ready.chunkPos = job.chunkPos;
    ready.buildTicket = job.buildTicket;
    ready.vertices = std::move(built.vertices);
    ready.indices = std::move(built.indices);
    {
        std::lock_guard<std::mutex> lock(m_readyChunkMeshesMutex);
        m_readyChunkMeshes.push_back(std::move(ready));
    }
}
