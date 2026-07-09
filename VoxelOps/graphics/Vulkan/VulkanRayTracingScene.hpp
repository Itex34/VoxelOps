#pragma once

#include <glm/fwd.hpp>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "../../render/ChunkMeshData.hpp"
#include "../../voxels/VoxelCoordHash.hpp"

class VulkanContext;
class UploadContext;
struct CpuChunkMesh;

class VulkanRayTracingScene {
public:
    struct StreamingStats {
        size_t pendingNormalUploads = 0;
        size_t pendingHighPriorityUploads = 0;
        size_t pendingTrackedUploads = 0;
        size_t pendingGpuBuildBatches = 0;
        bool pendingTlasBuild = false;
        size_t cachedBlasCount = 0;
        size_t activeBlasCount = 0;

        uint64_t lastBuildFrame = 0;
        size_t lastBuildSubmitted = 0;
        uint32_t lastBuildSelectedPrimitives = 0;
        VkDeviceSize lastBuildSelectedBytes = 0;
        bool lastBuildDeferredBudget = false;
        bool lastBuildDeferredOversized = false;
        uint32_t lastBuildDeferredPrimitives = 0;
        VkDeviceSize lastBuildDeferredBytes = 0;

        uint64_t lastTlasFrame = 0;
        bool lastTlasSubmitted = false;
        bool lastTlasSkippedPendingBuild = false;
        bool lastTlasSkippedEmpty = false;
        uint32_t lastTlasInstanceCount = 0;
    };

    VulkanRayTracingScene();
    ~VulkanRayTracingScene();

    VulkanRayTracingScene(const VulkanRayTracingScene &) = delete;
    VulkanRayTracingScene &operator=(const VulkanRayTracingScene &) = delete;
    VulkanRayTracingScene(VulkanRayTracingScene &&) = delete;
    VulkanRayTracingScene &operator=(VulkanRayTracingScene &&) = delete;

    void initialize(VulkanContext &context, UploadContext &uploadContext, uint64_t frameCounter);
    void collectRetiredResources(uint64_t frameCounter);
    void reset();

    bool uploadChunkGeometry(
        VulkanContext &context,
        UploadContext &uploadContext,
        uint64_t frameCounter,
        const glm::ivec3 &chunkPos,
        const CpuChunkMesh &cpuMesh,
        bool highPriority = false,
        bool activeForTracing = true
    );
    size_t processPendingUploads(
        VulkanContext &context,
        UploadContext &uploadContext,
        uint64_t frameCounter,
        size_t maxUploadsPerCall,
        uint32_t maxBuildPrimitives,
        VkDeviceSize maxBuildBytes,
        bool allowOversizedBuild,
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuChunkMeshes
    );
    void pollFinishedBlasBuilds(
        VulkanContext &context, uint64_t frameCounter, bool forcePoll = false
    );
    void pollFinishedTlasBuild(
        VulkanContext &context, uint64_t frameCounter, bool forcePoll = false
    );
    void removeChunkGeometry(uint64_t frameCounter, const glm::ivec3 &chunkPos);
    void updateActiveChunkRadius(
        uint64_t frameCounter, const glm::ivec3 &centerChunk, int radiusChunks
    );
    bool rebuild(VulkanContext &context, uint64_t frameCounter);
    [[nodiscard]] bool hasPendingBuildWork() const noexcept;
    [[nodiscard]] bool hasHighPriorityBuildWork() const noexcept;

    [[nodiscard]] bool isDirty() const noexcept {
        return m_dirty;
    }
    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] StreamingStats streamingStats() const noexcept;
    [[nodiscard]] VkAccelerationStructureKHR activeTlas() const noexcept {
        return m_activeTlas;
    }

private:
    struct RtSceneState;
    struct PendingChunkUpload {
        uint64_t revision = 0;
        bool highPriority = false;
    };

    std::unique_ptr<RtSceneState> m_state;
    std::deque<glm::ivec3> m_highPriorityChunkUploadQueue;
    std::deque<glm::ivec3> m_pendingChunkUploadQueue;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> m_pendingChunkUploadSet;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> m_highPriorityChunkUploadPending;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> m_cancelledChunkBuilds;
    std::unordered_map<glm::ivec3, PendingChunkUpload, IVec3Hash> m_pendingChunkUploads;
    bool m_highPriorityTlasRefreshRequested = false;
    bool m_dirty = false;
    VkAccelerationStructureKHR m_activeTlas = VK_NULL_HANDLE;
    StreamingStats m_streamingStats{};

    void trimInactiveBlasCache(uint64_t frameCounter);
};
