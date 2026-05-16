#include "ChunkMesher.hpp"

#include <iostream>

namespace {
    constexpr double kChunkMeshBuildLogThresholdMs = 2.0;
    constexpr double kChunkMeshUploadLogThresholdMs = 2.0;

    glm::vec3 decodePackedVoxelPosition(const VoxelVertex &packed) {
        const uint32_t x = (packed.low >> 0u) & 31u;
        const uint32_t y = (packed.low >> 5u) & 31u;
        const uint32_t z = (packed.low >> 10u) & 31u;
        return glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
} // namespace

ChunkMesher::ChunkMesher(ChunkManager &owner)
    : m_owner(owner) {
    ChunkMeshBuilder::resetProfileSnapshot();
}

ChunkMesher::~ChunkMesher() = default;

void ChunkMesher::markChunkDirty(const glm::ivec3 &pos) {
    enqueueDirtyChunk(pos, false);
}

void ChunkMesher::markChunkDirtyHighPriority(const glm::ivec3 &pos) {
    enqueueDirtyChunk(pos, true);
}

void ChunkMesher::enqueueDirtyChunk(const glm::ivec3 &pos, bool highPriority) {
    if (!m_owner.inBounds(pos)) {
        return;
    }
    auto it = m_owner.chunkMap.find(pos);
    if (it == m_owner.chunkMap.end()) {
        return;
    }

    it->second.dirty = true;
    if (m_dirtyChunkPending.insert(pos).second) {
        if (highPriority) {
            m_highPriorityDirtyChunkPending.insert(pos);
            m_highPriorityDirtyChunkQueue.push_back(pos);
        } else {
            m_dirtyChunkQueue.push_back(pos);
        }
        return;
    }

    if (highPriority && m_highPriorityDirtyChunkPending.insert(pos).second) {
        // Promote an already-pending chunk to the front-running queue.
        m_highPriorityDirtyChunkQueue.push_back(pos);
    }
}

std::optional<ChunkMesher::DirtyChunkWorkItem> ChunkMesher::dequeueDirtyChunk() {
    while (!m_highPriorityDirtyChunkQueue.empty()) {
        const glm::ivec3 pos = m_highPriorityDirtyChunkQueue.front();
        m_highPriorityDirtyChunkQueue.pop_front();
        if (m_highPriorityDirtyChunkPending.erase(pos) == 0) {
            continue;
        }
        if (m_dirtyChunkPending.erase(pos) == 0) {
            continue;
        }
        return DirtyChunkWorkItem{pos, true};
    }

    while (!m_dirtyChunkQueue.empty()) {
        const glm::ivec3 pos = m_dirtyChunkQueue.front();
        m_dirtyChunkQueue.pop_front();
        if (m_highPriorityDirtyChunkPending.find(pos) != m_highPriorityDirtyChunkPending.end()) {
            continue;
        }
        if (m_dirtyChunkPending.erase(pos) == 0) {
            continue;
        }
        return DirtyChunkWorkItem{pos, false};
    }

    return std::nullopt;
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
                    if (m_cpuChunkMeshes.erase(ready.chunkPos) > 0) {
                        ++m_cpuChunkMeshesVersion;
                    }
                } else {
                    CpuChunkMesh &cpuMesh = m_cpuChunkMeshes[ready.chunkPos];
                    cpuMesh.vertices = std::move(ready.vertices);
                    cpuMesh.rtVertices = std::move(ready.rtVertices);
                    cpuMesh.indices = std::move(ready.indices);
                    cpuMesh.revision = m_nextCpuChunkMeshRevision++;
                    cpuMesh.highPriorityRtBuild = ready.highPriority;
                    ++m_cpuChunkMeshesVersion;
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
            } else {
                enqueueDirtyChunk(ready.chunkPos, true);
            }
        }
    }

    while (!outOfBudget()) {
        if (maxChunksPerCall > 0 && scheduled >= maxChunksPerCall) {
            break;
        }

        const std::optional<DirtyChunkWorkItem> item = dequeueDirtyChunk();
        if (!item.has_value()) {
            break;
        }

        const glm::ivec3 &pos = item->chunkPos;

        auto it = m_owner.chunkMap.find(pos);
        if (it == m_owner.chunkMap.end()) {
            continue;
        }
        if (!it->second.dirty.load(std::memory_order_acquire)) {
            continue;
        }

        if (!requestChunkRebuild(pos, item->highPriority)) {
            auto chunkIt = m_owner.chunkMap.find(pos);
            if (chunkIt != m_owner.chunkMap.end() &&
                chunkIt->second.dirty.load(std::memory_order_acquire)) {
                enqueueDirtyChunk(pos, item->highPriority);
            }
        }
        ++scheduled;
    }
}

void ChunkMesher::updateDirtyChunkAt(const glm::ivec3 &chunkPos) {
    markChunkDirty(chunkPos);
    (void)requestChunkRebuild(chunkPos, false);
}

void ChunkMesher::onChunkRemoved(const glm::ivec3 &chunkPos) {
    m_chunkBuildTickets.erase(chunkPos);
    if (m_cpuChunkMeshes.erase(chunkPos) > 0) {
        ++m_cpuChunkMeshesVersion;
    }
    m_dirtyChunkPending.erase(chunkPos);
    m_highPriorityDirtyChunkPending.erase(chunkPos);
}

bool ChunkMesher::requestChunkRebuild(const glm::ivec3 &pos, bool highPriority) {
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
    job.highPriority = highPriority;
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
    ready.highPriority = job.highPriority;
    ready.rtVertices.reserve(built.vertices.size());
    for (const VoxelVertex &packed : built.vertices) {
        ready.rtVertices.push_back(decodePackedVoxelPosition(packed));
    }
    ready.vertices = std::move(built.vertices);
    ready.indices = std::move(built.indices);
    {
        std::lock_guard<std::mutex> lock(m_readyChunkMeshesMutex);
        m_readyChunkMeshes.push_back(std::move(ready));
    }
}
