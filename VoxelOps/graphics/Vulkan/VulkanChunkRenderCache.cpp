#include "VulkanChunkRenderCache.hpp"

#include "vulkan/VulkanContext.hpp"

#include <algorithm>
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

void VulkanChunkRenderCache::syncFromChunkManager(
    ChunkManager &chunkManager, const glm::ivec3 &cullingChunk, uint64_t frameCounter,
    VulkanContext &context, UploadContext &uploadContext,
    const std::function<void(const glm::ivec3 &)> &onChunkRemoved,
    const std::function<void(const glm::ivec3 &, const CpuChunkMesh &)> &onChunkUploaded) {
    static constexpr size_t kMaxChunkUploadsPerFrame = 8;

    const auto &cpuMeshes = chunkManager.getCpuChunkMeshes();
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
    };
    std::vector<UploadCandidate> candidates;
    candidates.reserve(kMaxChunkUploadsPerFrame);

    auto xzDistance2 = [&cullingChunk](const glm::ivec3 &chunkPos) -> int64_t {
        const glm::ivec3 d = chunkPos - cullingChunk;
        return static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
               static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
    };

    auto farthestIt = [&candidates]() {
        return std::max_element(
            candidates.begin(), candidates.end(),
            [](const UploadCandidate &a, const UploadCandidate &b) { return a.dist2 < b.dist2; });
    };

    for (const auto &[chunkPos, cpu] : cpuMeshes) {
        if (cpu.vertices.empty() || cpu.indices.empty()) {
            continue;
        }

        const auto cacheIt = m_chunkMeshes.find(chunkPos);
        if (cacheIt != m_chunkMeshes.end() && cacheIt->second.revision == cpu.revision &&
            cacheIt->second.mesh.getIndexCount() > 0) {
            continue;
        }

        const int64_t dist2 = xzDistance2(chunkPos);
        if (candidates.size() < kMaxChunkUploadsPerFrame) {
            candidates.push_back(UploadCandidate{chunkPos, dist2});
            continue;
        }

        auto it = farthestIt();
        if (it != candidates.end() && dist2 < it->dist2) {
            *it = UploadCandidate{chunkPos, dist2};
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
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const UploadCandidate &a, const UploadCandidate &b) { return a.dist2 < b.dist2; });

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

        CachedChunkMesh &cache = m_chunkMeshes[chunkPos];
        retireChunkMesh(std::move(cache.mesh), frameCounter);
        cache.mesh = VkMesh{};

        std::vector<VkMesh::PackedVoxelVertex> vertices;
        vertices.reserve(cpu.vertices.size());
        for (const VoxelVertex &packed : cpu.vertices) {
            VkMesh::PackedVoxelVertex out{};
            out.low = packed.low;
            out.high = packed.high;
            vertices.push_back(out);
        }
        std::vector<uint16_t> indices = cpu.indices;
        cache.mesh.setPackedVoxelGeometry(std::move(vertices), std::move(indices));
        cache.mesh.init(context.getDevice(), context.getPhysicalDevice(), uploadContext);
        cache.revision = cpu.revision;
        onChunkUploaded(chunkPos, cpu);
    }
}

void VulkanChunkRenderCache::cleanup() {
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
