#include "VulkanChunkRenderCache.hpp"

#include "vulkan/VulkanContext.hpp"

#include <algorithm>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <utility>

void VulkanChunkRenderCache::collectRetiredChunkMeshes(uint64_t frameCounter) {
    for (auto it = m_retiredChunkMeshes.begin(); it != m_retiredChunkMeshes.end();) {
        if (it->retireFrame > frameCounter) {
            ++it;
            continue;
        }
        it->mesh.cleanup();
        it = m_retiredChunkMeshes.erase(it);
    }
}

void VulkanChunkRenderCache::syncFromCpuChunkMeshes(
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes,
    const glm::ivec3 &cullingChunk,
    size_t maxChunkUploadsPerFrame,
    uint64_t frameCounter,
    VulkanContext &context,
    UploadContext &uploadContext,
    const std::function<void(const glm::ivec3 &)> &onChunkRemoved,
    const std::function<void(const glm::ivec3 &, const CpuChunkMesh &)> &onChunkUploaded
) {
    if (maxChunkUploadsPerFrame == 0) {
        maxChunkUploadsPerFrame = 1;
    }

    m_acceptBackgroundJobs = true;
    consumeCompletedUploadJobs(
        cpuMeshes, frameCounter, context, uploadContext, onChunkUploaded
    );

    std::vector<glm::ivec3> chunksToRemove;
    chunksToRemove.reserve(m_chunkMeshes.size());
    for (const auto &[chunkPos, _cached] : m_chunkMeshes) {
        if (cpuMeshes.find(chunkPos) == cpuMeshes.end()) {
            chunksToRemove.push_back(chunkPos);
        }
    }

    struct UploadCandidate {
        glm::ivec3 chunkPos{};
        int64_t dist2 = 0;
        bool highPriority = false;
    };
    std::vector<UploadCandidate> candidates;
    candidates.reserve(maxChunkUploadsPerFrame);

    auto xzDistance2 = [&cullingChunk](const glm::ivec3 &chunkPos) -> int64_t {
        const glm::ivec3 d = chunkPos - cullingChunk;
        return static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
               static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
    };

    auto farthestIt = [&candidates]() {
        return std::max_element(
            candidates.begin(),
            candidates.end(),
            [](const UploadCandidate &a, const UploadCandidate &b) {
                if (a.highPriority != b.highPriority) {
                    // High-priority edits should stay in the upload set ahead of distance-only work.
                    return a.highPriority;
                }
                return a.dist2 < b.dist2;
            }
        );
    };

    for (const auto &[chunkPos, cpu] : cpuMeshes) {
        if (cpu.vertices.empty() || cpu.indices.empty()) {
            // A chunk can remain present in the world map while its mesh becomes empty after edits.
            // In that case, retire any previously uploaded mesh/BLAS source immediately.
            const auto cachedIt = m_chunkMeshes.find(chunkPos);
            if (cachedIt != m_chunkMeshes.end()) {
                onChunkRemoved(chunkPos);
                retireChunkMesh(std::move(cachedIt->second.mesh), frameCounter);
                m_chunkMeshes.erase(cachedIt);
            }
            continue;
        }

        const auto cacheIt = m_chunkMeshes.find(chunkPos);
        if (cacheIt != m_chunkMeshes.end() && cacheIt->second.revision == cpu.revision &&
            cacheIt->second.mesh.getIndexCount() > 0) {
            continue;
        }

        const int64_t dist2 = xzDistance2(chunkPos);
        const bool highPriority = cpu.highPriorityRtBuild;
        if (candidates.size() < maxChunkUploadsPerFrame) {
            candidates.push_back(UploadCandidate{chunkPos, dist2, highPriority});
            continue;
        }

        auto it = farthestIt();
        if (it != candidates.end()) {
            const bool shouldReplace =
                (!highPriority && it->highPriority) ? false
                                                    : (highPriority && !it->highPriority) ||
                                                          (dist2 < it->dist2);
            if (shouldReplace) {
                *it = UploadCandidate{chunkPos, dist2, highPriority};
            }
        }
    }

    for (const glm::ivec3 &chunkPos : chunksToRemove) {
        auto it = m_chunkMeshes.find(chunkPos);
        if (it == m_chunkMeshes.end()) {
            continue;
        }
        onChunkRemoved(chunkPos);
        retireChunkMesh(std::move(it->second.mesh), frameCounter);
        m_chunkMeshes.erase(it);
        std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
        m_pendingChunkUploadRevisions.erase(chunkPos);
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const UploadCandidate &a, const UploadCandidate &b) {
            if (a.highPriority != b.highPriority) {
                return a.highPriority;
            }
            return a.dist2 < b.dist2;
        }
    );

    for (const UploadCandidate &candidate : candidates) {
        const glm::ivec3 &chunkPos = candidate.chunkPos;
        const auto cpuIt = cpuMeshes.find(chunkPos);
        if (cpuIt == cpuMeshes.end()) {
            continue;
        }

        const CpuChunkMesh &cpu = cpuIt->second;
        if (cpu.vertices.empty() || cpu.indices.empty()) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
            const auto pendingIt = m_pendingChunkUploadRevisions.find(chunkPos);
            if (pendingIt != m_pendingChunkUploadRevisions.end() &&
                pendingIt->second >= cpu.revision) {
                continue;
            }
        }

        enqueueBackgroundUploadJob(chunkPos, cpu);
    }
}

void VulkanChunkRenderCache::cleanup() {
    m_acceptBackgroundJobs = false;
    {
        auto drainPromise = std::make_shared<std::promise<void>>();
        std::future<void> drainFuture = drainPromise->get_future();
        m_chunkUploadWorkerPool.enqueue([drainPromise]() {
            drainPromise->set_value();
        });
        drainFuture.wait();
    }

    {
        std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
        m_pendingChunkUploadRevisions.clear();
        m_completedChunkUploadJobs.clear();
    }

    for (auto &[_, mesh] : m_chunkMeshes) {
        mesh.mesh.cleanup();
    }
    m_chunkMeshes.clear();

    for (auto &retired : m_retiredChunkMeshes) {
        retired.mesh.cleanup();
    }
    m_retiredChunkMeshes.clear();
}

void VulkanChunkRenderCache::retireChunkMesh(VkMesh &&mesh, uint64_t frameCounter) {
    static constexpr uint64_t kRetireDelayFrames = 24;
    RetiredChunkMesh retired{};
    retired.mesh = std::move(mesh);
    retired.retireFrame = frameCounter + kRetireDelayFrames;
    m_retiredChunkMeshes.push_back(std::move(retired));
}

void VulkanChunkRenderCache::enqueueBackgroundUploadJob(
    const glm::ivec3 &chunkPos, const CpuChunkMesh &cpuMesh
) {
    if (!m_acceptBackgroundJobs) {
        return;
    }

    PendingChunkUploadJob job{};
    job.chunkPos = chunkPos;
    job.revision = cpuMesh.revision;
    job.highPriority = cpuMesh.highPriorityRtBuild;
    job.vertices = cpuMesh.vertices;
    job.indices = cpuMesh.indices;
    {
        std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
        m_pendingChunkUploadRevisions[chunkPos] = cpuMesh.revision;
    }

    m_chunkUploadWorkerPool.enqueue([this, job = std::move(job)]() mutable {
        try {
            if (job.vertices.empty() || job.indices.empty()) {
                std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
                m_pendingChunkUploadRevisions.erase(job.chunkPos);
                return;
            }

            std::vector<VkMesh::PackedVoxelVertex> packedVertices;
            packedVertices.reserve(job.vertices.size());
            for (const VoxelVertex &packed : job.vertices) {
                VkMesh::PackedVoxelVertex out{};
                out.low = packed.low;
                out.high = packed.high;
                packedVertices.push_back(out);
            }

            CompletedChunkUploadJob completed{};
            completed.chunkPos = job.chunkPos;
            completed.revision = job.revision;
            completed.highPriority = job.highPriority;
            completed.packedVertices = std::move(packedVertices);
            completed.indices = std::move(job.indices);

            std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
            m_completedChunkUploadJobs.push_back(std::move(completed));
        } catch (const std::exception &e) {
            std::cerr << "[Vulkan][chunk-upload] background upload failed for chunk ("
                      << job.chunkPos.x << "," << job.chunkPos.y << "," << job.chunkPos.z
                      << "): " << e.what() << "\n";
            std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
            m_pendingChunkUploadRevisions.erase(job.chunkPos);
        }
    });
}

void VulkanChunkRenderCache::consumeCompletedUploadJobs(
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes,
    uint64_t frameCounter,
    VulkanContext &context,
    UploadContext &uploadContext,
    const std::function<void(const glm::ivec3 &, const CpuChunkMesh &)> &onChunkUploaded
) {
    std::deque<CompletedChunkUploadJob> completed;
    {
        std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
        if (m_completedChunkUploadJobs.empty()) {
            return;
        }
        completed.swap(m_completedChunkUploadJobs);
    }

    for (CompletedChunkUploadJob &job : completed) {
        {
            std::lock_guard<std::mutex> lock(m_chunkUploadStateMutex);
            auto pendingIt = m_pendingChunkUploadRevisions.find(job.chunkPos);
            if (pendingIt != m_pendingChunkUploadRevisions.end() &&
                pendingIt->second == job.revision) {
                m_pendingChunkUploadRevisions.erase(pendingIt);
            }
        }

        const auto cpuIt = cpuMeshes.find(job.chunkPos);
        if (cpuIt == cpuMeshes.end()) {
            continue;
        }
        const CpuChunkMesh &cpu = cpuIt->second;
        if (cpu.revision != job.revision) {
            continue;
        }

        VkMesh mesh{};
        mesh.setPackedVoxelGeometry(std::move(job.packedVertices), std::move(job.indices));
        mesh.init(context.getDevice(), context.getPhysicalDevice(), uploadContext);

        CachedChunkMesh &cache = m_chunkMeshes[job.chunkPos];
        retireChunkMesh(std::move(cache.mesh), frameCounter);
        cache.mesh = std::move(mesh);
        cache.revision = job.revision;
        onChunkUploaded(job.chunkPos, cpu);
    }
}
