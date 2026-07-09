#pragma once

#include "graphics/Mesh.hpp"
#include "../../render/ChunkMeshData.hpp"
#include "../../voxels/VoxelCoordHash.hpp"
#include "../../misc/ThreadPool.hpp"
#include "vulkan/UploadContext.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class VulkanContext;

class VulkanChunkRenderCache {
public:
    struct CachedChunkMesh {
        VkMesh mesh;
        uint64_t revision = 0;
    };

    void collectRetiredChunkMeshes(uint64_t frameCounter);
    void syncFromCpuChunkMeshes(
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes,
        const glm::ivec3 &cullingChunk,
        size_t maxChunkUploadsPerFrame,
        float uploadBudgetMs,
        uint64_t frameCounter,
        VulkanContext &context,
        UploadContext &uploadContext,
        const std::function<void(const glm::ivec3 &)> &onChunkRemoved,
        const std::function<void(const glm::ivec3 &, const CpuChunkMesh &)> &onChunkUploaded
    );
    void cleanup();

    [[nodiscard]] bool empty() const noexcept {
        return m_chunkMeshes.empty();
    }
    [[nodiscard]] size_t size() const noexcept {
        return m_chunkMeshes.size();
    }
    [[nodiscard]] const std::unordered_map<glm::ivec3, CachedChunkMesh, IVec3Hash> &
    getChunkMeshes() const noexcept {
        return m_chunkMeshes;
    }
    [[nodiscard]] uint64_t contentVersion() const noexcept {
        return m_contentVersion;
    }

private:
    struct PendingChunkUploadJob {
        glm::ivec3 chunkPos{};
        uint64_t revision = 0;
        bool highPriority = false;
        std::vector<VoxelVertex> vertices;
        std::vector<uint16_t> indices;
    };

    struct CompletedChunkUploadJob {
        glm::ivec3 chunkPos{};
        uint64_t revision = 0;
        bool highPriority = false;
        std::vector<VkMesh::PackedVoxelVertex> packedVertices;
        std::vector<uint16_t> indices;
    };

    struct RetiredChunkMesh {
        VkMesh mesh;
        uint64_t retireFrame = 0;
    };

    void enqueueBackgroundUploadJob(
        const glm::ivec3 &chunkPos, const CpuChunkMesh &cpuMesh
    );
    void consumeCompletedUploadJobs(
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes,
        uint64_t frameCounter,
        VulkanContext &context,
        UploadContext &uploadContext,
        const std::function<void(const glm::ivec3 &, const CpuChunkMesh &)> &onChunkUploaded,
        size_t maxCompletedUploadsPerCall,
        float uploadBudgetMs,
        const std::chrono::steady_clock::time_point &syncStart
    );
    void retireChunkMesh(VkMesh &&mesh, uint64_t frameCounter);

    std::mutex m_chunkUploadStateMutex;
    std::unordered_map<glm::ivec3, uint64_t, IVec3Hash> m_pendingChunkUploadRevisions;
    std::deque<CompletedChunkUploadJob> m_completedChunkUploadJobs;
    bool m_acceptBackgroundJobs = true;

    std::unordered_map<glm::ivec3, CachedChunkMesh, IVec3Hash> m_chunkMeshes;
    std::vector<RetiredChunkMesh> m_retiredChunkMeshes;
    uint64_t m_contentVersion = 0;
    ThreadPool m_chunkUploadWorkerPool{1};
};
