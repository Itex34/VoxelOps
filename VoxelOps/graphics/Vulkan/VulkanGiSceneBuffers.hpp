#pragma once

#include "VulkanChunkRenderCache.hpp"
#include "renderer/RenderFrameData.hpp"
#include "../../misc/ThreadPool.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../../voxels/Chunk.hpp"
#include "../../voxels/VoxelCoordHash.hpp"

class VulkanContext;

class VulkanGiSceneBuffers {
public:
    bool rebuild(
        const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &chunks,
        const VulkanChunkRenderCache &chunkRenderCache,
        VulkanContext &context,
        uint64_t frameCounter
    );
    void applyToLighting(GiLightingData &lighting) const;
    void collectRetiredBuffers(uint64_t frameCounter);
    [[nodiscard]] bool hasPendingBuildWork() const;
    void cleanup();

    [[nodiscard]] bool valid() const noexcept {
        return m_valid;
    }
    [[nodiscard]] size_t chunkCount() const noexcept {
        return m_chunkCount;
    }
    [[nodiscard]] uint64_t contentVersion() const noexcept {
        return m_contentVersion;
    }

private:
    struct GiBuildChunkSnapshotRef {
        glm::ivec3 chunkWorldBase{0};
        std::shared_ptr<std::array<BlockID, CHUNK_VOLUME>> blocks;
    };

    struct CachedChunkSnapshot {
        uint64_t revision = 0;
        std::shared_ptr<std::array<BlockID, CHUNK_VOLUME>> blocks;
    };

    struct GiBuildJob {
        uint64_t submitId = 0;
        uint64_t signatureXor = 0;
        uint64_t signatureSum = 0;
        size_t signatureCount = 0;
        glm::ivec3 minBlocks{0};
        glm::uvec3 dims{0u};
        uint32_t wordCount = 0;
        uint32_t voxelCount = 0;
        std::vector<GiBuildChunkSnapshotRef> chunks;
    };

    struct GiBuildResult {
        uint64_t submitId = 0;
        uint64_t signatureXor = 0;
        uint64_t signatureSum = 0;
        size_t signatureCount = 0;
        glm::ivec3 minBlocks{0};
        glm::uvec3 dims{0u};
        uint32_t wordCount = 0;
        uint32_t voxelCount = 0;
        std::vector<uint32_t> occupancyWords;
        std::vector<uint32_t> materialIds;
    };

    struct GiBufferSlot {
        vk::raii::Buffer occupancyBuffer{nullptr};
        vk::raii::DeviceMemory occupancyBufferMemory{nullptr};
        vk::DeviceSize occupancyCapacityBytes = 0;
        vk::raii::Buffer traceMaterialBuffer{nullptr};
        vk::raii::DeviceMemory traceMaterialBufferMemory{nullptr};
        vk::DeviceSize materialCapacityBytes = 0;
        uint64_t safeReuseAfterFrame = 0;
    };

    struct RetiredBuffers {
        vk::raii::Buffer occupancyBuffer{nullptr};
        vk::raii::DeviceMemory occupancyBufferMemory{nullptr};
        vk::DeviceSize occupancyCapacityBytes = 0;
        vk::raii::Buffer traceMaterialBuffer{nullptr};
        vk::raii::DeviceMemory traceMaterialBufferMemory{nullptr};
        vk::DeviceSize materialCapacityBytes = 0;
        uint64_t retireFrame = 0;
    };

    void ensureBufferSlotCapacity(
        GiBufferSlot &slot,
        VulkanContext &context,
        uint64_t frameCounter,
        vk::DeviceSize occupancyBytes,
        vk::DeviceSize materialBytes
    );
    void resetActiveState();
    bool consumeCompletedBuild(
        VulkanContext &context,
        uint64_t frameCounter
    );
    void enqueueBuildJob(GiBuildJob &&job);

    std::vector<GiBufferSlot> m_bufferSlots;
    size_t m_activeBufferSlot = static_cast<size_t>(-1);
    size_t m_nextBufferSlot = 0;
    glm::ivec3 m_minBlocks{0};
    glm::uvec3 m_dims{0u};
    glm::ivec4 m_worldBoundsXy{0};
    glm::ivec4 m_worldBoundsZ{0};
    uint32_t m_wordCount = 0;
    uint64_t m_signatureXor = 0;
    uint64_t m_signatureSum = 0;
    uint64_t m_contentVersion = 0;
    uint64_t m_lastRebuildFrame = 0;
    size_t m_chunkCount = 0;
    bool m_valid = false;
    std::vector<RetiredBuffers> m_retiredBuffers;
    std::unordered_map<glm::ivec3, CachedChunkSnapshot, IVec3Hash> m_chunkSnapshotCache;
    std::vector<uint32_t> m_hostOccupancyWords;
    std::vector<uint32_t> m_hostMaterialIds;

    mutable std::mutex m_buildStateMutex;
    std::deque<GiBuildResult> m_completedBuilds;
    bool m_acceptBuildJobs = true;
    bool m_buildInFlight = false;
    uint64_t m_inFlightSubmitId = 0;
    uint64_t m_nextSubmitId = 1;
    uint64_t m_lastAppliedSubmitId = 0;
    ThreadPool m_buildWorkerPool{1};
};
