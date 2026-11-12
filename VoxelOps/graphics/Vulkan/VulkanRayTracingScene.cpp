#include "VulkanRayTracingScene.hpp"
#include "RtResourceLifetime.hpp"

#include "../Mesh.hpp"
#include "../../voxels/Chunk.hpp"
#include "../../voxels/VoxelCoordHash.hpp"
#include "../../render/ChunkMeshData.hpp"
#include "vulkan/UploadContext.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"

#include <vk_mem_alloc.h>


#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    constexpr glm::ivec3 kRtDummyChunkPos{250000, 250000, 250000};
    constexpr vk::DeviceSize kRtBlasCacheBudgetBytes = 512ull * 1024ull * 1024ull;
    constexpr size_t kMaxPendingBlasBuildBatches = 1u;
    constexpr int kRtActiveRadiusHysteresisChunks = 3;

    bool sampleRtTiming(uint64_t frameCounter) {
        static const bool enabled = [] {
            const char *value = std::getenv("VOXELOPS_VK_TIMING_LOG");
            return value && (value[0] == '1' || value[0] == 't' || value[0] == 'T' ||
                             value[0] == 'y' || value[0] == 'Y');
        }();
        static const unsigned long interval = [] {
            const char *value = std::getenv("VOXELOPS_VK_RT_TIMING_INTERVAL");
            if (!value || !*value) {
                return 60ul;
            }
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            return end == value || parsed == 0 ? 60ul : parsed;
        }();
        return enabled && frameCounter % interval == 0;
    }

    bool isWithinRtChunkRadius(
        const glm::ivec3 &chunkPos, const glm::ivec3 &centerChunk, int radiusChunks
    ) {
        if (radiusChunks < 0) {
            return true;
        }
        const glm::ivec3 delta = chunkPos - centerChunk;
        const int64_t radius = static_cast<int64_t>(radiusChunks);
        return static_cast<int64_t>(delta.x) * static_cast<int64_t>(delta.x) +
                   static_cast<int64_t>(delta.z) * static_cast<int64_t>(delta.z) <=
               radius * radius;
    }

    struct DeviceBufferAllocation {
        VulkanBuffer buffer{};

        void reset() {
            buffer.destroy();
        }
    };

    void createBufferWithAddressing(
        VmaAllocator allocator,
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags properties,
        bool hostAccessSequentialWrite,
        const uint32_t *sharedQueueFamilies,
        uint32_t sharedQueueFamilyCount,
        DeviceBufferAllocation &outBuffer
    ) {
        const VmaAllocationCreateFlags allocationFlags =
            hostAccessSequentialWrite ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0;
        outBuffer.buffer.create(
            allocator,
            size,
            usage,
            properties,
            allocationFlags,
            sharedQueueFamilies,
            sharedQueueFamilyCount
        );
    }

    vk::DeviceSize nextPow2SizeClass(vk::DeviceSize value) {
        vk::DeviceSize rounded = 1;
        const vk::DeviceSize target = std::max<vk::DeviceSize>(value, 4u);
        while (rounded < target && rounded < (std::numeric_limits<vk::DeviceSize>::max() >> 1u)) {
            rounded <<= 1u;
        }
        return rounded;
    }

    struct ReusableBlasBuffer {
        DeviceBufferAllocation buffer{};
        vk::DeviceSize capacity = 0;
    };

    bool tryAcquireReusableBlasBuffer(
        std::vector<ReusableBlasBuffer> &pool,
        vk::DeviceSize minimumCapacity,
        DeviceBufferAllocation &outBuffer,
        vk::DeviceSize &outCapacity
    ) {
        if (pool.empty()) {
            return false;
        }

        auto bestIt = pool.end();
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if (it->capacity < minimumCapacity || it->buffer.buffer == VK_NULL_HANDLE) {
                continue;
            }
            if (bestIt == pool.end() || it->capacity < bestIt->capacity) {
                bestIt = it;
            }
        }
        if (bestIt == pool.end()) {
            return false;
        }

        outBuffer = std::move(bestIt->buffer);
        outCapacity = bestIt->capacity;
        pool.erase(bestIt);
        return true;
    }

    void recycleReusableBlasBuffer(
        std::vector<ReusableBlasBuffer> &pool,
        DeviceBufferAllocation &buffer,
        vk::DeviceSize capacity
    ) {
        if (buffer.buffer == VK_NULL_HANDLE || capacity == 0) {
            buffer.reset();
            return;
        }
        ReusableBlasBuffer reusable{};
        reusable.buffer = std::move(buffer);
        reusable.capacity = capacity;
        pool.push_back(std::move(reusable));
    }

    vk::DeviceAddress
    alignDeviceAddress(vk::DeviceAddress address, vk::DeviceSize alignment) {
        const vk::DeviceSize effectiveAlignment = std::max<vk::DeviceSize>(alignment, 1u);
        const vk::DeviceAddress mask = static_cast<vk::DeviceAddress>(effectiveAlignment - 1u);
        return (address + mask) & ~mask;
    }

    vk::DeviceSize getRtScratchAlignment(const vk::raii::PhysicalDevice &physicalDevice) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
        asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &asProps;
        vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(*physicalDevice), &props2);
        return std::max<vk::DeviceSize>(
            static_cast<vk::DeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment), 1u
        );
    }

    vk::DeviceAddress getBufferDeviceAddress(const vk::raii::Device &device, vk::Buffer buffer) {
        vk::BufferDeviceAddressInfo addressInfo{};
        addressInfo.buffer = buffer;
        return device.getBufferAddress(addressInfo);
    }

    VkTransformMatrixKHR makeTranslationTransform(const glm::vec3 &translation) {
        VkTransformMatrixKHR out{};
        out.matrix[0][0] = 1.0f;
        out.matrix[0][1] = 0.0f;
        out.matrix[0][2] = 0.0f;
        out.matrix[0][3] = translation.x;
        out.matrix[1][0] = 0.0f;
        out.matrix[1][1] = 1.0f;
        out.matrix[1][2] = 0.0f;
        out.matrix[1][3] = translation.y;
        out.matrix[2][0] = 0.0f;
        out.matrix[2][1] = 0.0f;
        out.matrix[2][2] = 1.0f;
        out.matrix[2][3] = translation.z;
        return out;
    }

    VoxelVertex makePackedVoxelVertex(uint32_t x, uint32_t y, uint32_t z) {
        VoxelVertex out{};
        out.low = ((x & 31u) << 0u) | ((y & 31u) << 5u) | ((z & 31u) << 10u);
        out.high = 0u;
        return out;
    }

} // namespace

struct VulkanRayTracingScene::RtSceneState {
    struct ChunkBlas {
        DeviceBufferAllocation vertexBuffer{};
        DeviceBufferAllocation indexBuffer{};
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        uint64_t revision = 0;
        bool fastTrace = false;
        uint32_t primitiveCount = 0;
        vk::DeviceSize vertexBytes = 0;
        vk::DeviceSize indexBytes = 0;
        vk::DeviceSize asBytes = 0;
        vk::DeviceSize vertexCapacity = 0;
        vk::DeviceSize indexCapacity = 0;
        vk::DeviceSize asCapacity = 0;
        uint64_t lastTouchedFrame = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            indexBuffer.reset();
            vertexBuffer.reset();
            revision = 0;
            fastTrace = false;
            primitiveCount = 0;
            vertexBytes = 0;
            indexBytes = 0;
            asBytes = 0;
            vertexCapacity = 0;
            indexCapacity = 0;
            asCapacity = 0;
            lastTouchedFrame = 0;
        }
    };

    struct RetiredChunkBlas {
        ChunkBlas blas{};
    };

    struct RetiredTlas {
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        vk::raii::Fence graphicsCompletion{nullptr};
        std::vector<vk::AccelerationStructureKHR> referencedBlases;
        vk::DeviceSize asCapacity = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            graphicsCompletion.clear();
            referencedBlases.clear();
            asCapacity = 0;
        }
    };

    struct ReusableTlasAsBuffer {
        DeviceBufferAllocation buffer{};
        vk::DeviceSize capacity = 0;

        void reset() {
            buffer.reset();
            capacity = 0;
        }
    };

    struct PendingGpuChunkBlas {
        glm::ivec3 chunkPos{0};
        RtBuildTicket ticket{};
        bool highPriority = false;
        ChunkBlas built{};
    };

    struct PendingGpuBuildBatch {
        vk::raii::Fence fence{nullptr};
        vk::raii::CommandBuffer commandBuffer{nullptr};
        vk::raii::QueryPool timingQueries{nullptr};
        uint32_t timestampValidBits = 0;
        float timestampPeriod = 0.0f;
        std::vector<PendingGpuChunkBlas> chunks;
        DeviceBufferAllocation vertexStagingBuffer{};
        vk::DeviceSize vertexStagingCapacity = 0;
        DeviceBufferAllocation indexStagingBuffer{};
        vk::DeviceSize indexStagingCapacity = 0;
        uint64_t submitFrame = 0;
    };

    struct PendingGpuTlasBuild {
        vk::raii::Fence fence{nullptr};
        vk::raii::CommandBuffer commandBuffer{nullptr};
        vk::raii::QueryPool timingQueries{nullptr};
        uint32_t timestampValidBits = 0;
        float timestampPeriod = 0.0f;
        uint32_t instanceCount = 0;
        bool highPriority = false;
        std::vector<vk::AccelerationStructureKHR> referencedBlases;
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        vk::DeviceSize asCapacity = 0;
        uint64_t submitFrame = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            asCapacity = 0;
            commandBuffer.clear();
            timingQueries.clear();
            fence.clear();
            submitFrame = 0;
        }
    };

    vk::raii::CommandPool commandPool{nullptr};
    ChunkBlas dummyBlas{};
    std::unordered_map<glm::ivec3, ChunkBlas, IVec3Hash> chunkBlases;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> activeChunkBlases;
    std::vector<RetiredChunkBlas> retiredChunkBlases;
    std::vector<ReusableBlasBuffer> reusableBlasVertexBuffers;
    std::vector<ReusableBlasBuffer> reusableBlasIndexBuffers;
    std::vector<ReusableBlasBuffer> reusableBlasAsBuffers;
    std::vector<ReusableBlasBuffer> reusableBlasVertexStagingBuffers;
    std::vector<ReusableBlasBuffer> reusableBlasIndexStagingBuffers;
    vk::DeviceSize residentBlasBytes = 0;
    vk::DeviceSize scratchAlignment = 0;
    DeviceBufferAllocation blasScratchBuffer{};
    vk::DeviceSize blasScratchBufferSize = 0;
    std::vector<PendingGpuBuildBatch> pendingGpuBuildBatches;

    DeviceBufferAllocation tlasAsBuffer{};
    vk::DeviceSize tlasAsBufferCapacity = 0;
    DeviceBufferAllocation tlasInstanceBuffer{};
    vk::DeviceSize tlasInstanceCapacity = 0;
    DeviceBufferAllocation tlasScratchBuffer{};
    vk::DeviceSize tlasScratchCapacity = 0;
    vk::raii::AccelerationStructureKHR tlas{nullptr};
    std::vector<vk::AccelerationStructureKHR> tlasReferencedBlases;
    std::vector<ReusableTlasAsBuffer> reusableTlasAsBuffers;
    std::vector<RetiredTlas> retiredTlases;
    std::optional<PendingGpuTlasBuild> pendingTlasBuild;

    bool ready = false;

    void reset() {
        tlas.clear();
        tlasReferencedBlases.clear();
        tlasAsBuffer.reset();
        tlasAsBufferCapacity = 0;
        tlasInstanceBuffer.reset();
        tlasInstanceCapacity = 0;
        tlasScratchBuffer.reset();
        tlasScratchCapacity = 0;
        if (pendingTlasBuild.has_value()) {
            pendingTlasBuild->reset();
            pendingTlasBuild.reset();
        }
        for (ReusableTlasAsBuffer &reusable : reusableTlasAsBuffers) {
            reusable.reset();
        }
        reusableTlasAsBuffers.clear();
        for (RetiredTlas &retired : retiredTlases) {
            retired.reset();
        }
        retiredTlases.clear();

        for (RetiredChunkBlas &retired : retiredChunkBlases) {
            retired.blas.reset();
        }
        retiredChunkBlases.clear();
        for (ReusableBlasBuffer &reusable : reusableBlasVertexBuffers) {
            reusable.buffer.reset();
            reusable.capacity = 0;
        }
        reusableBlasVertexBuffers.clear();
        for (ReusableBlasBuffer &reusable : reusableBlasIndexBuffers) {
            reusable.buffer.reset();
            reusable.capacity = 0;
        }
        reusableBlasIndexBuffers.clear();
        for (ReusableBlasBuffer &reusable : reusableBlasAsBuffers) {
            reusable.buffer.reset();
            reusable.capacity = 0;
        }
        reusableBlasAsBuffers.clear();
        for (ReusableBlasBuffer &reusable : reusableBlasVertexStagingBuffers) {
            reusable.buffer.reset();
            reusable.capacity = 0;
        }
        reusableBlasVertexStagingBuffers.clear();
        for (ReusableBlasBuffer &reusable : reusableBlasIndexStagingBuffers) {
            reusable.buffer.reset();
            reusable.capacity = 0;
        }
        reusableBlasIndexStagingBuffers.clear();

        for (auto &[_, chunkBlas] : chunkBlases) {
            chunkBlas.reset();
        }
        chunkBlases.clear();
        activeChunkBlases.clear();
        residentBlasBytes = 0;
        scratchAlignment = 0;
        dummyBlas.reset();
        blasScratchBuffer.reset();
        blasScratchBufferSize = 0;
        pendingGpuBuildBatches.clear();

        commandPool.clear();
        ready = false;
    }
};

VulkanRayTracingScene::VulkanRayTracingScene() = default;

VulkanRayTracingScene::~VulkanRayTracingScene() {
    reset();
}

bool VulkanRayTracingScene::isReady() const noexcept {
    return m_state && m_state->ready;
}

void VulkanRayTracingScene::trimInactiveBlasCache(uint64_t frameCounter) {
    (void)frameCounter;
    if (!m_state || !m_state->ready) {
        return;
    }

    RtSceneState &rt = *m_state;
    const auto chunkBlasBytes = [](const RtSceneState::ChunkBlas &blas) -> vk::DeviceSize {
        return blas.vertexBytes + blas.indexBytes + blas.asBytes;
    };
    const auto subtractResidentBytes = [&rt](vk::DeviceSize bytes) {
        rt.residentBlasBytes = (rt.residentBlasBytes > bytes) ? (rt.residentBlasBytes - bytes) : 0;
    };

    while (rt.residentBlasBytes > kRtBlasCacheBudgetBytes) {
        auto evictIt = rt.chunkBlases.end();
        for (auto it = rt.chunkBlases.begin(); it != rt.chunkBlases.end(); ++it) {
            if (it->first == kRtDummyChunkPos) {
                continue;
            }
            if (rt.activeChunkBlases.find(it->first) != rt.activeChunkBlases.end()) {
                continue;
            }
            if (evictIt == rt.chunkBlases.end() ||
                it->second.lastTouchedFrame < evictIt->second.lastTouchedFrame) {
                evictIt = it;
            }
        }

        if (evictIt == rt.chunkBlases.end()) {
            break;
        }

        RtSceneState::RetiredChunkBlas retired{};
        retired.blas = std::move(evictIt->second);
        rt.retiredChunkBlases.push_back(std::move(retired));
        subtractResidentBytes(chunkBlasBytes(rt.retiredChunkBlases.back().blas));
        rt.chunkBlases.erase(evictIt);
    }
}

void VulkanRayTracingScene::initialize(
    VulkanContext &context, UploadContext &uploadContext, uint64_t frameCounter
) {
    if (!context.isHardwareRayTracingSupported()) {
        reset();
        return;
    }

    if (!m_state) {
        m_state = std::make_unique<RtSceneState>();
    }

    RtSceneState &rt = *m_state;
    if (rt.commandPool == nullptr) {
        vk::CommandPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
        poolInfo.queueFamilyIndex = context.getRtBuildQueueFamily();
        rt.commandPool = vk::raii::CommandPool(context.getDevice(), poolInfo);
    }

    rt.ready = true;
    if (rt.scratchAlignment == 0) {
        rt.scratchAlignment = getRtScratchAlignment(context.getPhysicalDevice());
    }
    if (rt.chunkBlases.find(kRtDummyChunkPos) == rt.chunkBlases.end()) {
        CpuChunkMesh dummyMesh{};
        dummyMesh.vertices = {
            makePackedVoxelVertex(0u, 0u, 0u),
            makePackedVoxelVertex(1u, 0u, 0u),
            makePackedVoxelVertex(0u, 1u, 0u)
        };
        dummyMesh.rtVertices = {
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)
        };
        dummyMesh.indices = {0u, 1u, 2u};
        dummyMesh.revision = 1u;
        (void)uploadChunkGeometry(
            context, uploadContext, frameCounter, kRtDummyChunkPos, dummyMesh
        );
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> bootstrapCpuMeshes = {
            {kRtDummyChunkPos, dummyMesh}
        };
        (void)processPendingUploads(
            context,
            uploadContext,
            frameCounter,
            1u,
            std::numeric_limits<uint32_t>::max(),
            std::numeric_limits<VkDeviceSize>::max(),
            true,
            bootstrapCpuMeshes
        );
        while (!rt.pendingGpuBuildBatches.empty()) {
            const std::array<vk::Fence, 1> fences = {*rt.pendingGpuBuildBatches.front().fence};
            (void)context.getDevice().waitForFences(
                fences, vk::True, std::numeric_limits<uint64_t>::max()
            );
            pollFinishedBlasBuilds(context, frameCounter, true);
        }
    }
    if (!rt.chunkBlases.empty()) {
        m_dirty = true;
        (void)rebuild(context, frameCounter);
        if (rt.pendingTlasBuild.has_value()) {
            const std::array<vk::Fence, 1> fences = {*rt.pendingTlasBuild->fence};
            (void)context.getDevice().waitForFences(
                fences, vk::True, std::numeric_limits<uint64_t>::max()
            );
            pollFinishedTlasBuild(context, frameCounter, true);
        }
    }
}

void VulkanRayTracingScene::collectRetiredResources(VulkanContext &context) {
    if (!m_state) {
        return;
    }

    RtSceneState &rt = *m_state;
    // The fence follows the last graphics submission that could reference this TLAS.
    for (auto it = rt.retiredTlases.begin(); it != rt.retiredTlases.end();) {
        const std::array<vk::Fence, 1> fences = {*it->graphicsCompletion};
        const vk::Result status = context.getDevice().waitForFences(fences, vk::True, 0);
        if (status != vk::Result::eSuccess) {
            ++it;
            continue;
        }
        it->as.clear();
        if (it->asBuffer.buffer != VK_NULL_HANDLE && it->asCapacity > 0) {
            RtSceneState::ReusableTlasAsBuffer reusable{};
            reusable.buffer = std::move(it->asBuffer);
            reusable.capacity = it->asCapacity;
            rt.reusableTlasAsBuffers.push_back(std::move(reusable));
        }
        it = rt.retiredTlases.erase(it);
    }
    const auto referenced = [&rt](vk::AccelerationStructureKHR blas) {
        const auto *pending = rt.pendingTlasBuild.has_value()
                                  ? &rt.pendingTlasBuild->referencedBlases : nullptr;
        return rtBlasIsReferenced(
            blas, rt.tlasReferencedBlases, pending, rt.retiredTlases,
            [](const auto &tlas) -> const auto & { return tlas.referencedBlases; }
        );
    };
    for (auto it = rt.retiredChunkBlases.begin(); it != rt.retiredChunkBlases.end();) {
        if (it->blas.as != nullptr && referenced(*it->blas.as)) {
            ++it;
            continue;
        }

        it->blas.as.clear();
        const vk::DeviceSize vertexCapacity =
            (it->blas.vertexCapacity > 0) ? it->blas.vertexCapacity : it->blas.vertexBytes;
        const vk::DeviceSize indexCapacity =
            (it->blas.indexCapacity > 0) ? it->blas.indexCapacity : it->blas.indexBytes;
        const vk::DeviceSize asCapacity =
            (it->blas.asCapacity > 0) ? it->blas.asCapacity : it->blas.asBytes;
        recycleReusableBlasBuffer(
            rt.reusableBlasVertexBuffers, it->blas.vertexBuffer, vertexCapacity
        );
        recycleReusableBlasBuffer(
            rt.reusableBlasIndexBuffers, it->blas.indexBuffer, indexCapacity
        );
        recycleReusableBlasBuffer(rt.reusableBlasAsBuffers, it->blas.asBuffer, asCapacity);
        it->blas.reset();
        it = rt.retiredChunkBlases.erase(it);
    }
}

void VulkanRayTracingScene::reset() {
    m_activeTlas = VK_NULL_HANDLE;
    m_dirty = false;
    m_highPriorityTlasRefreshRequested = false;
    m_pendingChunkUploads.clear();
    m_upgradeQuietSince = {};
    m_lastUpgradeTime = {};
    if (!m_state) {
        return;
    }
    m_state->reset();
    m_state.reset();
}

bool VulkanRayTracingScene::uploadChunkGeometry(
    VulkanContext &context,
    UploadContext &uploadContext,
    uint64_t frameCounter,
    const glm::ivec3 &chunkPos,
    const CpuChunkMesh &cpuMesh,
    bool highPriority,
    bool activeForTracing
) {
    (void)context;
    (void)uploadContext;
    (void)frameCounter;
    if (!m_state || !m_state->ready) {
        return false;
    }
    if (cpuMesh.rtVertices.empty() || cpuMesh.indices.empty()) {
        return false;
    }

    RtSceneState &rt = *m_state;
    const uint64_t geometryRevision =
        cpuMesh.rtGeometryRevision != 0 ? cpuMesh.rtGeometryRevision : cpuMesh.revision;
    auto existingIt = rt.chunkBlases.find(chunkPos);
    if (!activeForTracing) {
        if (existingIt != rt.chunkBlases.end()) {
            existingIt->second.lastTouchedFrame = frameCounter;
        }
        const bool wasActive = (rt.activeChunkBlases.erase(chunkPos) > 0);
        m_pendingChunkUploads.erase(chunkPos);
        if (wasActive) {
            m_dirty = true;
        }
        return existingIt != rt.chunkBlases.end();
    }

    if (existingIt != rt.chunkBlases.end() && existingIt->second.revision == geometryRevision) {
        existingIt->second.lastTouchedFrame = frameCounter;
        const bool becameActive = rt.activeChunkBlases.insert(chunkPos).second;
        if (becameActive) {
            m_dirty = true;
            if (highPriority) {
                m_highPriorityTlasRefreshRequested = true;
            }
        }
        return true;
    }

    rt.activeChunkBlases.insert(chunkPos);

    for (auto &batch : rt.pendingGpuBuildBatches) {
        for (auto &build : batch.chunks) {
            if (build.chunkPos == chunkPos && build.ticket.matches(geometryRevision)) {
                build.highPriority = build.highPriority || highPriority;
                return true;
            }
        }
    }

    auto pendingIt = m_pendingChunkUploads.find(chunkPos);
    if (pendingIt != m_pendingChunkUploads.end() &&
        pendingIt->second.revision == geometryRevision) {
        pendingIt->second.highPriority = pendingIt->second.highPriority || highPriority;
        return true;
    }

    m_pendingChunkUploads[chunkPos] = PendingChunkUpload{geometryRevision, highPriority};
    return true;
}

void VulkanRayTracingScene::setBuildPriorityCenter(const glm::ivec3 &centerChunk) noexcept {
    m_buildPriorityCenter = centerChunk;
}

void VulkanRayTracingScene::scheduleBlasUpgrade(
    bool frameHasHeadroom,
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuChunkMeshes
) {
    static const bool upgradesEnabled = [] {
        const char *value = std::getenv("VOXELOPS_RT_BLAS_UPGRADES");
        return value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y');
    }();
    if (!upgradesEnabled) {
        return;
    }
    constexpr auto quietTime = std::chrono::seconds(1);
    constexpr auto upgradeInterval = std::chrono::seconds(1);
    // Limit a single, non-preemptible build. Larger BLASes remain fast-build.
    constexpr uint32_t maxUpgradePrimitives = 512;
    const bool hasNormalUpload = std::any_of(
        m_pendingChunkUploads.begin(), m_pendingChunkUploads.end(),
        [](const auto &entry) { return !entry.second.upgrade; }
    );
    if (!frameHasHeadroom || m_dirty || hasNormalUpload) {
        for (auto it = m_pendingChunkUploads.begin(); it != m_pendingChunkUploads.end();) {
            if (it->second.upgrade) {
                it = m_pendingChunkUploads.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!m_state || !m_state->ready || !frameHasHeadroom || m_dirty ||
        hasPendingBuildWork()) {
        m_upgradeQuietSince = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_upgradeQuietSince == std::chrono::steady_clock::time_point{}) {
        m_upgradeQuietSince = now;
        return;
    }
    if (now - m_upgradeQuietSince < quietTime ||
        now - m_lastUpgradeTime < upgradeInterval) {
        return;
    }
    RtSceneState &rt = *m_state;
    auto selected = rt.chunkBlases.end();
    int64_t selectedDistance = std::numeric_limits<int64_t>::max();
    for (auto it = rt.chunkBlases.begin(); it != rt.chunkBlases.end(); ++it) {
        const auto &blas = it->second;
        if (it->first == kRtDummyChunkPos || blas.fastTrace ||
            blas.primitiveCount > maxUpgradePrimitives ||
            !rt.activeChunkBlases.contains(it->first) ||
            rt.residentBlasBytes + 2u * blas.asCapacity > kRtBlasCacheBudgetBytes) {
            continue;
        }
        const auto mesh = cpuChunkMeshes.find(it->first);
        if (mesh == cpuChunkMeshes.end() || mesh->second.rtVertices.empty() ||
            mesh->second.indices.empty()) {
            continue;
        }
        const uint64_t revision = mesh->second.rtGeometryRevision != 0
            ? mesh->second.rtGeometryRevision : mesh->second.revision;
        if (revision != blas.revision) {
            continue;
        }
        const int64_t x = static_cast<int64_t>(it->first.x) - m_buildPriorityCenter.x;
        const int64_t z = static_cast<int64_t>(it->first.z) - m_buildPriorityCenter.z;
        const int64_t distance = x * x + z * z;
        if (selected == rt.chunkBlases.end() || distance < selectedDistance) {
            selected = it;
            selectedDistance = distance;
        }
    }
    if (selected != rt.chunkBlases.end()) {
        m_pendingChunkUploads.emplace(
            selected->first, PendingChunkUpload{selected->second.revision, false, true}
        );
        m_lastUpgradeTime = now;
    }
}

size_t VulkanRayTracingScene::processPendingUploads(
    VulkanContext &context,
    UploadContext &uploadContext,
    uint64_t frameCounter,
    size_t maxUploadsPerCall,
    uint32_t maxBuildPrimitives,
    VkDeviceSize maxBuildBytes,
    bool allowOversizedBuild,
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuChunkMeshes
) {
    (void)uploadContext;
    m_streamingStats.lastBuildFrame = frameCounter;
    m_streamingStats.lastBuildSubmitted = 0;
    m_streamingStats.lastBuildSelectedPrimitives = 0;
    m_streamingStats.lastBuildSelectedBytes = 0;
    m_streamingStats.lastBuildDeferredBudget = false;
    m_streamingStats.lastBuildDeferredOversized = false;
    m_streamingStats.lastBuildDeferredPrimitives = 0;
    m_streamingStats.lastBuildDeferredBytes = 0;
    if (!m_state || !m_state->ready || maxUploadsPerCall == 0) {
        return 0;
    }
    RtSceneState &rt = *m_state;
    if (rt.pendingGpuBuildBatches.size() >= kMaxPendingBlasBuildBatches) {
        return 0;
    }

    struct PendingBuild {
        RtSceneState::PendingGpuChunkBlas pending{};
        vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
        vk::AccelerationStructureGeometryKHR geometry{};
        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        const std::vector<glm::vec3> *sourceVertices = nullptr;
        const std::vector<uint16_t> *sourceIndices = nullptr;
        vk::DeviceSize vertexBytes = 0;
        vk::DeviceSize indexBytes = 0;
        vk::DeviceSize vertexOffset = 0;
        vk::DeviceSize indexOffset = 0;
    };

    const vk::raii::Device &device = context.getDevice();
    if (rt.scratchAlignment == 0) {
        rt.scratchAlignment = getRtScratchAlignment(context.getPhysicalDevice());
    }
    const vk::DeviceSize rtScratchAlignment = rt.scratchAlignment;
    const VmaAllocator vmaAllocator = context.getVmaAllocator();
    const std::array<uint32_t, 2> rtGraphicsQueueFamilies = {
        context.getRtBuildQueueFamily(),
        context.getGraphicsQueueFamily()
    };
    const uint32_t *sharedQueueFamilies =
        context.hasDedicatedRtBuildQueue() ? rtGraphicsQueueFamilies.data() : nullptr;
    const uint32_t sharedQueueFamilyCount = context.hasDedicatedRtBuildQueue() ? 2u : 0u;
    const auto allocateBlasDeviceBuffer = [&](std::vector<ReusableBlasBuffer> &pool,
                                              vk::DeviceSize requestedSize,
                                              vk::BufferUsageFlags usage,
                                              DeviceBufferAllocation &outBuffer,
                                              vk::DeviceSize &outCapacity) {
        const vk::DeviceSize minimumCapacity = nextPow2SizeClass(requestedSize);
        if (tryAcquireReusableBlasBuffer(pool, minimumCapacity, outBuffer, outCapacity)) {
            return;
        }
        createBufferWithAddressing(
            vmaAllocator,
            minimumCapacity,
            usage,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            sharedQueueFamilies,
            sharedQueueFamilyCount,
            outBuffer
        );
        outCapacity = minimumCapacity;
    };
    const auto allocateBlasStagingBuffer = [&](std::vector<ReusableBlasBuffer> &pool,
                                               vk::DeviceSize requestedSize,
                                               DeviceBufferAllocation &outBuffer,
                                               vk::DeviceSize &outCapacity) {
        const vk::DeviceSize minimumCapacity = nextPow2SizeClass(requestedSize);
        if (tryAcquireReusableBlasBuffer(pool, minimumCapacity, outBuffer, outCapacity)) {
            return;
        }
        createBufferWithAddressing(
            vmaAllocator,
            minimumCapacity,
            vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            true,
            nullptr,
            0u,
            outBuffer
        );
        outCapacity = minimumCapacity;
    };

    std::vector<PendingBuild> pendingBuilds;
    pendingBuilds.reserve(maxUploadsPerCall);
    size_t processed = 0;
    uint32_t selectedPrimitives = 0;
    vk::DeviceSize selectedBytes = 0;
    vk::DeviceSize maxScratchSize = 0;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> deferredOversized;
    const auto requeuePendingUpload = [&](const glm::ivec3 &chunkPos,
                                          const PendingChunkUpload &pendingUpload,
                                          bool highPriorityUpload) {
        m_pendingChunkUploads[chunkPos] = pendingUpload;
        m_pendingChunkUploads[chunkPos].highPriority = highPriorityUpload;
    };

    const auto distanceSquared = [this](const glm::ivec3 &pos) {
        const int64_t x = static_cast<int64_t>(pos.x) - m_buildPriorityCenter.x;
        const int64_t y = static_cast<int64_t>(pos.y) - m_buildPriorityCenter.y;
        const int64_t z = static_cast<int64_t>(pos.z) - m_buildPriorityCenter.z;
        return x * x + y * y + z * z;
    };
    while (processed < maxUploadsPerCall && !m_pendingChunkUploads.empty()) {
        auto pendingIt = m_pendingChunkUploads.end();
        for (auto it = m_pendingChunkUploads.begin(); it != m_pendingChunkUploads.end(); ++it) {
            if (deferredOversized.contains(it->first)) {
                continue;
            }
            if (pendingIt == m_pendingChunkUploads.end()) {
                pendingIt = it;
                continue;
            }
            const bool candidateMissing = !rt.chunkBlases.contains(it->first);
            const bool selectedMissing = !rt.chunkBlases.contains(pendingIt->first);
            if ((it->second.upgrade != pendingIt->second.upgrade)
                    ? !it->second.upgrade
                    : (it->second.highPriority != pendingIt->second.highPriority)
                    ? it->second.highPriority
                    : ((candidateMissing != selectedMissing)
                           ? candidateMissing
                           : distanceSquared(it->first) < distanceSquared(pendingIt->first))) {
                pendingIt = it;
            }
        }
        if (pendingIt == m_pendingChunkUploads.end()) {
            break;
        }
        const glm::ivec3 chunkPos = pendingIt->first;
        PendingChunkUpload pendingUpload = std::move(pendingIt->second);
        m_pendingChunkUploads.erase(pendingIt);
        const bool highPriorityUpload = pendingUpload.highPriority;

        auto cpuMeshIt = cpuChunkMeshes.find(chunkPos);
        if (cpuMeshIt == cpuChunkMeshes.end()) {
            const bool wasActive = (rt.activeChunkBlases.erase(chunkPos) > 0);
            if (wasActive) {
                m_dirty = true;
            }
            ++processed;
            continue;
        }
        const CpuChunkMesh &cpuMesh = cpuMeshIt->second;
        const uint64_t geometryRevision =
            cpuMesh.rtGeometryRevision != 0 ? cpuMesh.rtGeometryRevision : cpuMesh.revision;
        if (geometryRevision != pendingUpload.revision) {
            ++processed;
            continue;
        }

        auto existingIt = rt.chunkBlases.find(chunkPos);
        if (existingIt != rt.chunkBlases.end() && existingIt->second.revision == geometryRevision &&
            !pendingUpload.upgrade) {
            existingIt->second.lastTouchedFrame = frameCounter;
            ++processed;
            continue;
        }
        if (pendingUpload.upgrade &&
            (existingIt == rt.chunkBlases.end() || existingIt->second.revision != geometryRevision ||
             existingIt->second.fastTrace || !rt.activeChunkBlases.contains(chunkPos))) {
            ++processed;
            continue;
        }
        if (cpuMesh.rtVertices.empty() || cpuMesh.indices.empty()) {
            const bool wasActive = (rt.activeChunkBlases.erase(chunkPos) > 0);
            if (wasActive) {
                m_dirty = true;
            }
            ++processed;
            continue;
        }

        PendingBuild build{};
        build.pending.chunkPos = chunkPos;
        build.pending.ticket.revision = geometryRevision;
        build.pending.highPriority = highPriorityUpload;
        build.sourceVertices = &cpuMesh.rtVertices;
        build.sourceIndices = &cpuMesh.indices;
        build.vertexBytes =
            static_cast<vk::DeviceSize>(build.sourceVertices->size() * sizeof(glm::vec3));
        build.indexBytes =
            static_cast<vk::DeviceSize>(build.sourceIndices->size() * sizeof(uint16_t));

        const uint32_t primitiveCount = static_cast<uint32_t>(build.sourceIndices->size() / 3u);
        if (primitiveCount == 0u) {
            ++processed;
            continue;
        }
        const vk::DeviceSize buildUploadBytes = build.vertexBytes + build.indexBytes;
        const bool primitiveBudgetExceeded =
            maxBuildPrimitives > 0u && selectedPrimitives > 0u &&
            selectedPrimitives + primitiveCount > maxBuildPrimitives;
        const bool byteBudgetExceeded =
            maxBuildBytes > 0u && selectedBytes > 0u &&
            selectedBytes + buildUploadBytes > maxBuildBytes;
        const bool singleBuildOverBudget =
            (maxBuildPrimitives > 0u && primitiveCount > maxBuildPrimitives) ||
            (maxBuildBytes > 0u && buildUploadBytes > maxBuildBytes);
        if (primitiveBudgetExceeded || byteBudgetExceeded ||
            (singleBuildOverBudget && !highPriorityUpload && !allowOversizedBuild)) {
            m_streamingStats.lastBuildDeferredBudget =
                primitiveBudgetExceeded || byteBudgetExceeded;
            m_streamingStats.lastBuildDeferredOversized = singleBuildOverBudget;
            m_streamingStats.lastBuildDeferredPrimitives = primitiveCount;
            m_streamingStats.lastBuildDeferredBytes = buildUploadBytes;
            requeuePendingUpload(chunkPos, pendingUpload, highPriorityUpload);
            if (singleBuildOverBudget && !highPriorityUpload && !allowOversizedBuild &&
                selectedPrimitives == 0u) {
                deferredOversized.insert(chunkPos);
                continue;
            }
            break;
        }

        allocateBlasDeviceBuffer(
            rt.reusableBlasVertexBuffers,
            build.vertexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            build.pending.built.vertexBuffer,
            build.pending.built.vertexCapacity
        );
        allocateBlasDeviceBuffer(
            rt.reusableBlasIndexBuffers,
            build.indexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            build.pending.built.indexBuffer,
            build.pending.built.indexCapacity
        );

        const vk::DeviceAddress vertexAddress = getBufferDeviceAddress(
            device, static_cast<vk::Buffer>(build.pending.built.vertexBuffer.buffer.handle())
        );
        const vk::DeviceAddress indexAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(build.pending.built.indexBuffer.buffer.handle()));

        build.triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
        build.triangles.vertexData.deviceAddress = vertexAddress;
        build.triangles.vertexStride = sizeof(glm::vec3);
        build.triangles.maxVertex = static_cast<uint32_t>(build.sourceVertices->size() - 1u);
        build.triangles.indexType = vk::IndexType::eUint16;
        build.triangles.indexData.deviceAddress = indexAddress;

        build.geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
        build.geometry.flags = vk::GeometryFlagBitsKHR::eOpaque;
        build.geometry.geometry.triangles = build.triangles;

        build.buildInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        build.buildInfo.flags = pendingUpload.upgrade
            ? vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
            : vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
        build.buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        build.buildInfo.geometryCount = 1;
        build.buildInfo.pGeometries = &build.geometry;

        const std::array<uint32_t, 1> primitiveCounts = {primitiveCount};
        const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo =
            device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, build.buildInfo, primitiveCounts
            );
        maxScratchSize = std::max(maxScratchSize, sizeInfo.buildScratchSize);

        allocateBlasDeviceBuffer(
            rt.reusableBlasAsBuffers,
            sizeInfo.accelerationStructureSize,
            vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            build.pending.built.asBuffer,
            build.pending.built.asCapacity
        );
        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = static_cast<vk::Buffer>(build.pending.built.asBuffer.buffer.handle());
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        build.pending.built.as = vk::raii::AccelerationStructureKHR(device, asCreateInfo);
        build.pending.built.revision = build.pending.ticket.revision;
        build.pending.built.fastTrace = pendingUpload.upgrade;
        build.pending.built.primitiveCount = primitiveCount;
        build.pending.built.vertexBytes = build.pending.built.vertexCapacity;
        build.pending.built.indexBytes = build.pending.built.indexCapacity;
        build.pending.built.asBytes = build.pending.built.asCapacity;
        build.pending.built.lastTouchedFrame = frameCounter;
        build.rangeInfo.primitiveCount = primitiveCount;

        pendingBuilds.push_back(std::move(build));
        pendingBuilds.back().buildInfo.pGeometries = &pendingBuilds.back().geometry;
        selectedPrimitives += primitiveCount;
        selectedBytes += buildUploadBytes;
        ++processed;
    }

    if (pendingBuilds.empty()) {
        return processed;
    }
    m_streamingStats.lastBuildSubmitted = pendingBuilds.size();
    m_streamingStats.lastBuildSelectedPrimitives = selectedPrimitives;
    m_streamingStats.lastBuildSelectedBytes = selectedBytes;

    DeviceBufferAllocation batchVertexStaging{};
    DeviceBufferAllocation batchIndexStaging{};
    vk::DeviceSize batchVertexStagingCapacity = 0;
    vk::DeviceSize batchIndexStagingCapacity = 0;
    vk::DeviceSize totalVertexBytes = 0;
    vk::DeviceSize totalIndexBytes = 0;
    for (PendingBuild &build : pendingBuilds) {
        build.vertexOffset = totalVertexBytes;
        build.indexOffset = totalIndexBytes;
        totalVertexBytes += build.vertexBytes;
        totalIndexBytes += build.indexBytes;
    }
    const auto parallelCopySlices = [&](auto &&copyRangeFn) {
        if (pendingBuilds.empty()) {
            return;
        }
        const size_t buildCount = pendingBuilds.size();
        const unsigned hwThreads = std::max(1u, std::thread::hardware_concurrency());
        const size_t maxWorkers = std::min<size_t>(static_cast<size_t>(hwThreads), buildCount);
        constexpr size_t kMinBuildsPerWorker = 16u;
        const size_t workerCount = std::max<size_t>(
            1u, std::min(maxWorkers, buildCount / kMinBuildsPerWorker)
        );
        if (workerCount <= 1u) {
            copyRangeFn(0u, buildCount);
            return;
        }

        const size_t sliceSize = (buildCount + workerCount - 1u) / workerCount;
        std::vector<std::future<void>> futures;
        futures.reserve(workerCount - 1u);
        size_t sliceStart = 0u;
        for (size_t worker = 0u; worker + 1u < workerCount; ++worker) {
            const size_t sliceEnd = std::min(buildCount, sliceStart + sliceSize);
            futures.push_back(std::async(
                std::launch::async,
                [sliceStart, sliceEnd, &copyRangeFn]() { copyRangeFn(sliceStart, sliceEnd); }
            ));
            sliceStart = sliceEnd;
        }
        copyRangeFn(sliceStart, buildCount);
        for (auto &future : futures) {
            future.get();
        }
    };

    if (totalVertexBytes > 0) {
        allocateBlasStagingBuffer(
            rt.reusableBlasVertexStagingBuffers,
            totalVertexBytes,
            batchVertexStaging,
            batchVertexStagingCapacity
        );
        void *mapped = batchVertexStaging.buffer.map();
        if (mapped == nullptr) {
            throw std::runtime_error("Failed to map batched vertex staging buffer.");
        }
        uint8_t *cursor = static_cast<uint8_t *>(mapped);
        parallelCopySlices([&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const PendingBuild &build = pendingBuilds[i];
                if (build.vertexBytes == 0 || build.sourceVertices == nullptr) {
                    continue;
                }
                std::memcpy(
                    cursor + static_cast<size_t>(build.vertexOffset),
                    build.sourceVertices->data(),
                    static_cast<size_t>(build.vertexBytes)
                );
            }
        });
        batchVertexStaging.buffer.unmap();
    }
    if (totalIndexBytes > 0) {
        allocateBlasStagingBuffer(
            rt.reusableBlasIndexStagingBuffers,
            totalIndexBytes,
            batchIndexStaging,
            batchIndexStagingCapacity
        );
        void *mapped = batchIndexStaging.buffer.map();
        if (mapped == nullptr) {
            throw std::runtime_error("Failed to map batched index staging buffer.");
        }
        uint8_t *cursor = static_cast<uint8_t *>(mapped);
        parallelCopySlices([&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const PendingBuild &build = pendingBuilds[i];
                if (build.indexBytes == 0 || build.sourceIndices == nullptr) {
                    continue;
                }
                std::memcpy(
                    cursor + static_cast<size_t>(build.indexOffset),
                    build.sourceIndices->data(),
                    static_cast<size_t>(build.indexBytes)
                );
            }
        });
        batchIndexStaging.buffer.unmap();
    }

    const vk::DeviceSize requiredScratchSize = std::max<vk::DeviceSize>(maxScratchSize, 4u);
    const vk::DeviceSize requiredScratchBufferSize =
        requiredScratchSize + (rtScratchAlignment - 1u);
    if (requiredScratchBufferSize > rt.blasScratchBufferSize ||
        rt.blasScratchBuffer.buffer == VK_NULL_HANDLE) {
        rt.blasScratchBuffer.reset();
        createBufferWithAddressing(
            vmaAllocator,
            requiredScratchBufferSize,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            nullptr,
            0u,
            rt.blasScratchBuffer
        );
        rt.blasScratchBufferSize = requiredScratchBufferSize;
    }

    try {
        const vk::DeviceAddress scratchBaseAddress = getBufferDeviceAddress(
            device, static_cast<vk::Buffer>(rt.blasScratchBuffer.buffer.handle())
        );
        const vk::DeviceAddress scratchAddress =
            alignDeviceAddress(scratchBaseAddress, rtScratchAlignment);
        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        vk::raii::QueryPool timingQueries{nullptr};
        uint32_t timestampValidBits = 0;
        float timestampPeriod = 0.0f;
        if (sampleRtTiming(frameCounter)) {
            const auto queueFamilies = context.getPhysicalDevice().getQueueFamilyProperties();
            timestampValidBits = queueFamilies[context.getRtBuildQueueFamily()].timestampValidBits;
            timestampPeriod = context.getPhysicalDevice().getProperties().limits.timestampPeriod;
        }
        if (timestampValidBits != 0) {
            vk::QueryPoolCreateInfo queryInfo{};
            queryInfo.queryType = vk::QueryType::eTimestamp;
            queryInfo.queryCount = 3;
            timingQueries = vk::raii::QueryPool(device, queryInfo);
            commandBuffer.resetQueryPool(*timingQueries, 0, 3);
            commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *timingQueries, 0);
        }
        for (size_t i = 0; i < pendingBuilds.size(); ++i) {
            PendingBuild &build = pendingBuilds[i];
            if (build.vertexBytes == 0 || build.indexBytes == 0) {
                throw std::runtime_error("BLAS upload payload unexpectedly empty.");
            }

            vk::BufferCopy vertexCopy{};
            vertexCopy.srcOffset = build.vertexOffset;
            vertexCopy.size = build.vertexBytes;
            const std::array<vk::BufferCopy, 1> vertexRegions = {vertexCopy};
            commandBuffer.copyBuffer(
                static_cast<vk::Buffer>(batchVertexStaging.buffer.handle()),
                static_cast<vk::Buffer>(build.pending.built.vertexBuffer.buffer.handle()),
                vertexRegions
            );

            vk::BufferCopy indexCopy{};
            indexCopy.srcOffset = build.indexOffset;
            indexCopy.size = build.indexBytes;
            const std::array<vk::BufferCopy, 1> indexRegions = {indexCopy};
            commandBuffer.copyBuffer(
                static_cast<vk::Buffer>(batchIndexStaging.buffer.handle()),
                static_cast<vk::Buffer>(build.pending.built.indexBuffer.buffer.handle()),
                indexRegions
            );

            vk::BufferMemoryBarrier vertexBarrier{};
            vertexBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            vertexBarrier.dstAccessMask = vk::AccessFlagBits::eAccelerationStructureReadKHR;
            vertexBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vertexBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vertexBarrier.buffer =
                static_cast<vk::Buffer>(build.pending.built.vertexBuffer.buffer.handle());
            vertexBarrier.offset = 0;
            vertexBarrier.size = VK_WHOLE_SIZE;
            vk::BufferMemoryBarrier indexBarrier{};
            indexBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            indexBarrier.dstAccessMask = vk::AccessFlagBits::eAccelerationStructureReadKHR;
            indexBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            indexBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            indexBarrier.buffer =
                static_cast<vk::Buffer>(build.pending.built.indexBuffer.buffer.handle());
            indexBarrier.offset = 0;
            indexBarrier.size = VK_WHOLE_SIZE;
            const std::array<vk::BufferMemoryBarrier, 2> copyBarriers = {vertexBarrier, indexBarrier};
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
                {},
                {},
                copyBarriers,
                {}
            );

            if (timingQueries != nullptr && i == 0) {
                commandBuffer.writeTimestamp(
                    vk::PipelineStageFlagBits::eTransfer, *timingQueries, 1
                );
            }

            build.buildInfo.dstAccelerationStructure = *build.pending.built.as;
            build.buildInfo.scratchData.deviceAddress = scratchAddress;
            const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {
                &build.rangeInfo
            }; 
            const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {
                build.buildInfo
            };
            commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
            if (i + 1 < pendingBuilds.size()) {
                vk::MemoryBarrier buildBarrier{};
                buildBarrier.srcAccessMask = vk::AccessFlagBits::eAccelerationStructureWriteKHR;
                buildBarrier.dstAccessMask = vk::AccessFlagBits::eAccelerationStructureReadKHR |
                                             vk::AccessFlagBits::eAccelerationStructureWriteKHR;
                const std::array<vk::MemoryBarrier, 1> memoryBarriers = {buildBarrier};
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
                    vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
                    {},
                    memoryBarriers,
                    {},
                    {}
                );
            }
        }
        if (timingQueries != nullptr) {
            commandBuffer.writeTimestamp(
                vk::PipelineStageFlagBits::eBottomOfPipe, *timingQueries, 2
            );
        }
        commandBuffer.end();
        RtSceneState::PendingGpuBuildBatch batch{};
        batch.fence = vk::raii::Fence(device, vk::FenceCreateInfo{});
        batch.commandBuffer = std::move(commandBuffer);
        batch.timingQueries = std::move(timingQueries);
        batch.timestampValidBits = timestampValidBits;
        batch.timestampPeriod = timestampPeriod;
        batch.submitFrame = frameCounter;
        batch.chunks.reserve(pendingBuilds.size());
        for (PendingBuild &build : pendingBuilds) {
            batch.chunks.push_back(std::move(build.pending));
        }
        if (batchVertexStaging.buffer != VK_NULL_HANDLE) {
            batch.vertexStagingCapacity = batchVertexStagingCapacity;
            batch.vertexStagingBuffer = std::move(batchVertexStaging);
        }
        if (batchIndexStaging.buffer != VK_NULL_HANDLE) {
            batch.indexStagingCapacity = batchIndexStagingCapacity;
            batch.indexStagingBuffer = std::move(batchIndexStaging);
        }

        const vk::CommandBuffer rawCommandBuffer = *batch.commandBuffer;
        vk::SubmitInfo submitInfo{};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &rawCommandBuffer;
        context.getRtBuildQueue().submit(submitInfo, *batch.fence);
        rt.pendingGpuBuildBatches.push_back(std::move(batch));
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to build BLAS batch: " << e.what() << "\n";
        return 0;
    }
    return pendingBuilds.size();
}

void VulkanRayTracingScene::pollFinishedBlasBuilds(
    VulkanContext &context, uint64_t frameCounter, bool forcePoll,
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> *cpuChunkMeshes
) {
    if (!m_state || !m_state->ready) {
        return;
    }

    RtSceneState &rt = *m_state;
    const auto chunkBlasBytes = [](const RtSceneState::ChunkBlas &blas) -> vk::DeviceSize {
        return blas.vertexBytes + blas.indexBytes + blas.asBytes;
    };
    const auto subtractResidentBytes = [&rt](vk::DeviceSize bytes) {
        rt.residentBlasBytes = (rt.residentBlasBytes > bytes) ? (rt.residentBlasBytes - bytes) : 0;
    };
    const auto recycleChunkBlasBuffers = [&rt](RtSceneState::ChunkBlas &blas) {
        blas.as.clear();
        const vk::DeviceSize vertexCapacity =
            (blas.vertexCapacity > 0) ? blas.vertexCapacity : blas.vertexBytes;
        const vk::DeviceSize indexCapacity =
            (blas.indexCapacity > 0) ? blas.indexCapacity : blas.indexBytes;
        const vk::DeviceSize asCapacity = (blas.asCapacity > 0) ? blas.asCapacity : blas.asBytes;
        recycleReusableBlasBuffer(rt.reusableBlasVertexBuffers, blas.vertexBuffer, vertexCapacity);
        recycleReusableBlasBuffer(rt.reusableBlasIndexBuffers, blas.indexBuffer, indexCapacity);
        recycleReusableBlasBuffer(rt.reusableBlasAsBuffers, blas.asBuffer, asCapacity);
        blas.reset();
    };
    const auto recycleBatchStagingBuffers = [&rt](RtSceneState::PendingGpuBuildBatch &batch) {
        recycleReusableBlasBuffer(
            rt.reusableBlasVertexStagingBuffers,
            batch.vertexStagingBuffer,
            batch.vertexStagingCapacity
        );
        recycleReusableBlasBuffer(
            rt.reusableBlasIndexStagingBuffers,
            batch.indexStagingBuffer,
            batch.indexStagingCapacity
        );
        batch.vertexStagingCapacity = 0;
        batch.indexStagingCapacity = 0;
    };
    const vk::raii::Device &device = context.getDevice();
    for (auto it = rt.pendingGpuBuildBatches.begin(); it != rt.pendingGpuBuildBatches.end();) {
        if (!forcePoll && frameCounter <= it->submitFrame) {
            ++it;
            continue;
        }
        const std::array<vk::Fence, 1> fences = {*it->fence};
        const vk::Result status = device.waitForFences(fences, vk::True, 0);
        if (status == vk::Result::eTimeout) {
            ++it;
            continue;
        }
        if (status != vk::Result::eSuccess) {
            std::cerr << "[Vulkan][RT] BLAS batch fence failed status="
                      << static_cast<int>(status) << "\n";
            for (auto &chunkBuild : it->chunks) {
                recycleChunkBlasBuffers(chunkBuild.built);
            }
            recycleBatchStagingBuffers(*it);
            it = rt.pendingGpuBuildBatches.erase(it);
            continue;
        }

        if (it->timingQueries != nullptr) {
            std::array<uint64_t, 3> timestamps{};
            const VkResult result = vkGetQueryPoolResults(
                static_cast<VkDevice>(*device), static_cast<VkQueryPool>(*it->timingQueries),
                0, 3, sizeof(timestamps), timestamps.data(), sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT
            );
            if (result == VK_SUCCESS) {
                const uint64_t mask = it->timestampValidBits >= 64
                                          ? std::numeric_limits<uint64_t>::max()
                                          : (uint64_t{1} << it->timestampValidBits) - 1u;
                const uint64_t elapsed = (timestamps[2] - timestamps[0]) & mask;
                const uint64_t uploadElapsed = (timestamps[1] - timestamps[0]) & mask;
                uint64_t rtPrimitives = 0;
                for (const auto &chunk : it->chunks) {
                    rtPrimitives += chunk.built.primitiveCount;
                }
                std::cout << "[Vulkan][Timing][BLAS] frame=" << it->submitFrame
                          << " chunks=" << it->chunks.size()
                          << " upgrades=" << std::count_if(it->chunks.begin(), it->chunks.end(),
                                 [](const auto &chunk) { return chunk.built.fastTrace; })
                          << " queueElapsedMs=" << (static_cast<double>(elapsed) * it->timestampPeriod / 1e6)
                          << " uploadQueueElapsedMs=" << (static_cast<double>(uploadElapsed) * it->timestampPeriod / 1e6)
                          << " rtPrimitives=" << rtPrimitives
                          << "\n";
            }
        }

        bool sceneChanged = false;
        bool highPrioritySceneChanged = false;
        for (auto &chunkBuild : it->chunks) {
            if (cpuChunkMeshes != nullptr && chunkBuild.chunkPos != kRtDummyChunkPos) {
                const auto meshIt = cpuChunkMeshes->find(chunkBuild.chunkPos);
                if (meshIt == cpuChunkMeshes->end() || meshIt->second.rtVertices.empty() ||
                    meshIt->second.indices.empty()) {
                    recycleChunkBlasBuffers(chunkBuild.built);
                    continue;
                }
                const auto &mesh = meshIt->second;
                const uint64_t currentRevision =
                    mesh.rtGeometryRevision != 0 ? mesh.rtGeometryRevision : mesh.revision;
                if (!chunkBuild.ticket.matches(currentRevision)) {
                    recycleChunkBlasBuffers(chunkBuild.built);
                    continue;
                }
            }
            const auto pendingIt = m_pendingChunkUploads.find(chunkBuild.chunkPos);
            const std::optional<uint64_t> queuedRevision =
                pendingIt != m_pendingChunkUploads.end()
                    ? std::optional<uint64_t>{pendingIt->second.revision} : std::nullopt;
            if (!chunkBuild.ticket.canPublish(queuedRevision)) {
                recycleChunkBlasBuffers(chunkBuild.built);
                continue;
            }
            if (chunkBuild.built.fastTrace) {
                const auto existingIt = rt.chunkBlases.find(chunkBuild.chunkPos);
                const std::optional<uint64_t> currentRevision = existingIt == rt.chunkBlases.end()
                    ? std::nullopt : std::optional<uint64_t>{existingIt->second.revision};
                if (!rtUpgradeCanPublish(
                        chunkBuild.ticket, currentRevision,
                        existingIt != rt.chunkBlases.end() && existingIt->second.fastTrace,
                        rt.activeChunkBlases.contains(chunkBuild.chunkPos),
                        pendingIt != m_pendingChunkUploads.end())) {
                    recycleChunkBlasBuffers(chunkBuild.built);
                    continue;
                }
            }

            // Build inputs are no longer read once the batch fence signals. Keep
            // them in the pool for the next chunk instead of retaining two GPU
            // buffers for the lifetime of every resident acceleration structure.
            recycleReusableBlasBuffer(
                rt.reusableBlasVertexBuffers, chunkBuild.built.vertexBuffer,
                chunkBuild.built.vertexCapacity
            );
            recycleReusableBlasBuffer(
                rt.reusableBlasIndexBuffers, chunkBuild.built.indexBuffer,
                chunkBuild.built.indexCapacity
            );
            chunkBuild.built.vertexBytes = 0;
            chunkBuild.built.indexBytes = 0;
            chunkBuild.built.vertexCapacity = 0;
            chunkBuild.built.indexCapacity = 0;

            auto existingIt = rt.chunkBlases.find(chunkBuild.chunkPos);
            if (existingIt != rt.chunkBlases.end()) {
                RtSceneState::RetiredChunkBlas retired{};
                subtractResidentBytes(chunkBlasBytes(existingIt->second));
                retired.blas = std::move(existingIt->second);
                rt.retiredChunkBlases.push_back(std::move(retired));
                rt.chunkBlases.erase(existingIt);
            }
            chunkBuild.built.lastTouchedFrame = frameCounter;
            rt.residentBlasBytes += chunkBlasBytes(chunkBuild.built);
            rt.chunkBlases.insert_or_assign(chunkBuild.chunkPos, std::move(chunkBuild.built));
            if (rt.activeChunkBlases.find(chunkBuild.chunkPos) != rt.activeChunkBlases.end()) {
                sceneChanged = true;
                highPrioritySceneChanged = highPrioritySceneChanged || chunkBuild.highPriority;
            }
        }
        if (sceneChanged) {
            m_dirty = true;
            if (highPrioritySceneChanged) {
                m_highPriorityTlasRefreshRequested = true;
            }
        }
        recycleBatchStagingBuffers(*it);
        it = rt.pendingGpuBuildBatches.erase(it);
    }

    trimInactiveBlasCache(frameCounter);
}

void VulkanRayTracingScene::removeChunkGeometry(uint64_t frameCounter, const glm::ivec3 &chunkPos) {
    if (!m_state) {
        return;
    }

    m_pendingChunkUploads.erase(chunkPos);

    RtSceneState &rt = *m_state;
    for (auto &batch : rt.pendingGpuBuildBatches) {
        for (auto &build : batch.chunks) {
            if (build.chunkPos == chunkPos) {
                build.ticket.cancelled = true;
            }
        }
    }
    const bool wasActive = (rt.activeChunkBlases.erase(chunkPos) > 0);
    auto it = rt.chunkBlases.find(chunkPos);
    if (it != rt.chunkBlases.end()) {
        it->second.lastTouchedFrame = frameCounter;
    }
    if (wasActive) {
        m_dirty = true;
    }
    trimInactiveBlasCache(frameCounter);
}

void VulkanRayTracingScene::updateActiveChunkRadius(
    uint64_t frameCounter, const glm::ivec3 &centerChunk, int radiusChunks,
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuChunkMeshes
) {
    if (!m_state || !m_state->ready) {
        return;
    }

    RtSceneState &rt = *m_state;
    bool changed = false;
    const auto hasGeometry = [&cpuChunkMeshes](const glm::ivec3 &pos) {
        const auto it = cpuChunkMeshes.find(pos);
        return it != cpuChunkMeshes.end() && !it->second.rtVertices.empty() &&
               !it->second.indices.empty();
    };
    for (auto it = m_pendingChunkUploads.begin(); it != m_pendingChunkUploads.end();) {
        if (!hasGeometry(it->first)) {
            it = m_pendingChunkUploads.erase(it);
        } else {
            ++it;
        }
    }
    for (auto &batch : rt.pendingGpuBuildBatches) {
        for (auto &build : batch.chunks) {
            if (build.chunkPos != kRtDummyChunkPos && !hasGeometry(build.chunkPos)) {
                build.ticket.cancelled = true;
            }
        }
    }
    const int pruneRadiusChunks =
        (radiusChunks < 0) ? radiusChunks : radiusChunks + kRtActiveRadiusHysteresisChunks;
    for (auto it = rt.activeChunkBlases.begin(); it != rt.activeChunkBlases.end();) {
        if (*it == kRtDummyChunkPos ||
            (hasGeometry(*it) && isWithinRtChunkRadius(*it, centerChunk, pruneRadiusChunks))) {
            ++it;
            continue;
        }
        it = rt.activeChunkBlases.erase(it);
        changed = true;
    }

    for (auto &[chunkPos, chunkBlas] : rt.chunkBlases) {
        const auto meshIt = cpuChunkMeshes.find(chunkPos);
        if (chunkPos == kRtDummyChunkPos || meshIt == cpuChunkMeshes.end() ||
            meshIt->second.rtVertices.empty() || meshIt->second.indices.empty() ||
            !isWithinRtChunkRadius(chunkPos, centerChunk, radiusChunks)) {
            continue;
        }
        chunkBlas.lastTouchedFrame = frameCounter;
        if (rt.activeChunkBlases.insert(chunkPos).second) {
            changed = true;
        }
    }

    if (changed) {
        m_dirty = true;
    }
    trimInactiveBlasCache(frameCounter);
}

bool VulkanRayTracingScene::hasPendingBuildWork() const noexcept {
    if (!m_state || !m_state->ready) {
        return !m_pendingChunkUploads.empty();
    }
    return !m_pendingChunkUploads.empty() ||
           !m_state->pendingGpuBuildBatches.empty() || m_state->pendingTlasBuild.has_value();
}

bool VulkanRayTracingScene::hasHighPriorityBuildWork() const noexcept {
    if (std::any_of(m_pendingChunkUploads.begin(), m_pendingChunkUploads.end(),
                    [](const auto &entry) { return entry.second.highPriority; })) {
        return true;
    }
    if (!m_state || !m_state->ready) {
        return false;
    }
    bool hasHighPriorityInFlight = false;
    for (const RtSceneState::PendingGpuBuildBatch &batch : m_state->pendingGpuBuildBatches) {
        for (const RtSceneState::PendingGpuChunkBlas &chunk : batch.chunks) {
            if (chunk.highPriority) {
                hasHighPriorityInFlight = true;
                break;
            }
        }
        if (hasHighPriorityInFlight) {
            break;
        }
    }
    const bool highPriorityTlasRequested = m_highPriorityTlasRefreshRequested && m_dirty;
    return hasHighPriorityInFlight || highPriorityTlasRequested;
}

bool VulkanRayTracingScene::rebuild(VulkanContext &context, uint64_t frameCounter) {
    m_streamingStats.lastTlasFrame = frameCounter;
    m_streamingStats.lastTlasSubmitted = false;
    m_streamingStats.lastTlasSkippedPendingBuild = false;
    m_streamingStats.lastTlasSkippedEmpty = false;
    m_streamingStats.lastTlasInstanceCount = 0;
    if (!m_state || !m_state->ready) {
        m_activeTlas = VK_NULL_HANDLE;
        m_dirty = false;
        return false;
    }

    try {
        RtSceneState &rt = *m_state;
        const vk::raii::Device &device = context.getDevice();
        if (rt.scratchAlignment == 0) {
            rt.scratchAlignment = getRtScratchAlignment(context.getPhysicalDevice());
        }
        const vk::DeviceSize rtScratchAlignment = rt.scratchAlignment;
        const VmaAllocator vmaAllocator = context.getVmaAllocator();
        const std::array<uint32_t, 2> rtGraphicsQueueFamilies = {
            context.getRtBuildQueueFamily(),
            context.getGraphicsQueueFamily()
        };
        const uint32_t *sharedQueueFamilies =
            context.hasDedicatedRtBuildQueue() ? rtGraphicsQueueFamilies.data() : nullptr;
        const uint32_t sharedQueueFamilyCount = context.hasDedicatedRtBuildQueue() ? 2u : 0u;

        if (rt.pendingTlasBuild.has_value()) {
            m_streamingStats.lastTlasSkippedPendingBuild = true;
            return false;
        }

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        std::vector<vk::AccelerationStructureKHR> references;
        instances.reserve(rt.activeChunkBlases.size());
        references.reserve(rt.activeChunkBlases.size());
        for (const glm::ivec3 &chunkPos : rt.activeChunkBlases) {
            auto chunkIt = rt.chunkBlases.find(chunkPos);
            if (chunkIt == rt.chunkBlases.end()) {
                continue;
            }
            RtSceneState::ChunkBlas &chunkBlas = chunkIt->second;
            if (chunkBlas.as == nullptr || chunkBlas.primitiveCount == 0u) {
                continue;
            }

            vk::AccelerationStructureDeviceAddressInfoKHR addrInfo{};
            addrInfo.accelerationStructure = *chunkBlas.as;
            const vk::DeviceAddress blasAddress =
                device.getAccelerationStructureAddressKHR(addrInfo);
            if (blasAddress == 0) {
                continue;
            }

            VkAccelerationStructureInstanceKHR instance{};
            instance.transform = makeTranslationTransform(glm::vec3(chunkPos * CHUNK_SIZE));
            instance.instanceCustomIndex = 0u;
            instance.mask = 0xFFu;
            instance.instanceShaderBindingTableRecordOffset = 0u;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference = blasAddress;
            instances.push_back(instance);
            references.push_back(*chunkBlas.as);
            chunkBlas.lastTouchedFrame = frameCounter;
        }

        if (instances.empty()) {
            m_streamingStats.lastTlasSkippedEmpty = true;
            m_dirty = false;
            return false;
        }
        m_streamingStats.lastTlasInstanceCount = static_cast<uint32_t>(
            std::min<size_t>(instances.size(), std::numeric_limits<uint32_t>::max())
        );

        RtSceneState::PendingGpuTlasBuild pending{};
        pending.referencedBlases = std::move(references);
        pending.highPriority = m_highPriorityTlasRefreshRequested;
        const vk::DeviceSize instanceBytes = static_cast<vk::DeviceSize>(
            std::max<size_t>(1u, instances.size()) * sizeof(VkAccelerationStructureInstanceKHR)
        );
        if (rt.tlasInstanceBuffer.buffer == VK_NULL_HANDLE || instanceBytes > rt.tlasInstanceCapacity) {
            rt.tlasInstanceBuffer.reset();
            createBufferWithAddressing(
                vmaAllocator,
                instanceBytes,
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                    vk::BufferUsageFlagBits::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                true,
                sharedQueueFamilies,
                sharedQueueFamilyCount,
                rt.tlasInstanceBuffer
            );
            rt.tlasInstanceCapacity = instanceBytes;
        }
        if (!instances.empty()) {
            void *mapped = rt.tlasInstanceBuffer.buffer.map();
            if (mapped == nullptr) {
                throw std::runtime_error("Failed to map TLAS instance buffer.");
            }
            std::memcpy(
                mapped,
                instances.data(),
                static_cast<size_t>(instances.size() * sizeof(VkAccelerationStructureInstanceKHR))
            );
            rt.tlasInstanceBuffer.buffer.unmap();
        }

        vk::AccelerationStructureGeometryInstancesDataKHR instancesData{};
        instancesData.arrayOfPointers = VK_FALSE;
        instancesData.data.deviceAddress = getBufferDeviceAddress(
            device, static_cast<vk::Buffer>(rt.tlasInstanceBuffer.buffer.handle())
        );

        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.geometryType = vk::GeometryTypeKHR::eInstances;
        geometry.geometry.instances = instancesData;

        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        const uint32_t primitiveCount = static_cast<uint32_t>(instances.size());
        const std::array<uint32_t, 1> primitiveCounts = {primitiveCount};
        const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo =
            device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, primitiveCounts
            );

        const vk::DeviceSize asRequiredSize =
            std::max<vk::DeviceSize>(sizeInfo.accelerationStructureSize, 4u);
        auto reusableIt = rt.reusableTlasAsBuffers.end();
        for (auto it = rt.reusableTlasAsBuffers.begin(); it != rt.reusableTlasAsBuffers.end(); ++it) {
            if (it->capacity >= asRequiredSize &&
                (reusableIt == rt.reusableTlasAsBuffers.end() || it->capacity < reusableIt->capacity)) {
                reusableIt = it;
            }
        }
        if (reusableIt != rt.reusableTlasAsBuffers.end()) {
            pending.asBuffer = std::move(reusableIt->buffer);
            pending.asCapacity = reusableIt->capacity;
            rt.reusableTlasAsBuffers.erase(reusableIt);
        } else {
            createBufferWithAddressing(
                vmaAllocator,
                asRequiredSize,
                vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                    vk::BufferUsageFlagBits::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal,
                false,
                sharedQueueFamilies,
                sharedQueueFamilyCount,
                pending.asBuffer
            );
            pending.asCapacity = asRequiredSize;
        }

        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = static_cast<vk::Buffer>(pending.asBuffer.buffer.handle());
        asCreateInfo.size = asRequiredSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        pending.as = vk::raii::AccelerationStructureKHR(device, asCreateInfo);

        const vk::DeviceSize scratchRequiredSize =
            std::max<vk::DeviceSize>(sizeInfo.buildScratchSize, 4u);
        const vk::DeviceSize scratchBufferRequiredSize =
            scratchRequiredSize + (rtScratchAlignment - 1u);
        if (rt.tlasScratchBuffer.buffer == VK_NULL_HANDLE ||
            scratchBufferRequiredSize > rt.tlasScratchCapacity) {
            rt.tlasScratchBuffer.reset();
            createBufferWithAddressing(
                vmaAllocator,
                scratchBufferRequiredSize,
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal,
                false,
                nullptr,
                0u,
                rt.tlasScratchBuffer
            );
            rt.tlasScratchCapacity = scratchBufferRequiredSize;
        }

        buildInfo.dstAccelerationStructure = *pending.as;
        const vk::DeviceAddress tlasScratchBaseAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(rt.tlasScratchBuffer.buffer.handle()));
        buildInfo.scratchData.deviceAddress =
            alignDeviceAddress(tlasScratchBaseAddress, rtScratchAlignment);

        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {&rangeInfo};
        const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {buildInfo};

        pending.commandBuffer = VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        if (sampleRtTiming(frameCounter)) {
            const auto families = context.getPhysicalDevice().getQueueFamilyProperties();
            pending.timestampValidBits = families[context.getRtBuildQueueFamily()].timestampValidBits;
            pending.timestampPeriod = context.getPhysicalDevice().getProperties().limits.timestampPeriod;
            if (pending.timestampValidBits != 0) {
                vk::QueryPoolCreateInfo queryInfo{};
                queryInfo.queryType = vk::QueryType::eTimestamp;
                queryInfo.queryCount = 2;
                pending.timingQueries = vk::raii::QueryPool(device, queryInfo);
                pending.commandBuffer.resetQueryPool(*pending.timingQueries, 0, 2);
                pending.commandBuffer.writeTimestamp(
                    vk::PipelineStageFlagBits::eTopOfPipe, *pending.timingQueries, 0
                );
            }
        }
        pending.commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
        if (pending.timingQueries != nullptr) {
            pending.commandBuffer.writeTimestamp(
                vk::PipelineStageFlagBits::eBottomOfPipe, *pending.timingQueries, 1
            );
        }
        pending.commandBuffer.end();
        pending.fence = vk::raii::Fence(device, vk::FenceCreateInfo{});
        const vk::CommandBuffer rawCommandBuffer = *pending.commandBuffer;
        vk::SubmitInfo submitInfo{};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &rawCommandBuffer;
        context.getRtBuildQueue().submit(submitInfo, *pending.fence);
        pending.submitFrame = frameCounter;
        pending.instanceCount = primitiveCount;
        rt.pendingTlasBuild = std::move(pending);
        m_streamingStats.lastTlasSubmitted = true;
        m_highPriorityTlasRefreshRequested = false;
        m_dirty = false;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to rebuild TLAS: " << e.what() << "\n";
        return false;
    }
}

VulkanRayTracingScene::StreamingStats VulkanRayTracingScene::streamingStats() const noexcept {
    StreamingStats stats = m_streamingStats;
    stats.pendingHighPriorityUploads = static_cast<size_t>(std::count_if(
        m_pendingChunkUploads.begin(), m_pendingChunkUploads.end(),
        [](const auto &entry) { return entry.second.highPriority; }
    ));
    stats.pendingNormalUploads = m_pendingChunkUploads.size() - stats.pendingHighPriorityUploads;
    stats.pendingTrackedUploads = m_pendingChunkUploads.size();
    if (m_state && m_state->ready) {
        stats.pendingGpuBuildBatches = m_state->pendingGpuBuildBatches.size();
        stats.pendingTlasBuild = m_state->pendingTlasBuild.has_value();
        stats.cachedBlasCount = m_state->chunkBlases.size();
        stats.fastTraceBlasCount = std::count_if(
            m_state->chunkBlases.begin(), m_state->chunkBlases.end(),
            [](const auto &entry) { return entry.second.fastTrace; }
        );
        stats.activeRequestedCount = m_state->activeChunkBlases.size();
        stats.publishedBlasCount = m_state->tlasReferencedBlases.size();
        stats.activeBlasCount = static_cast<size_t>(std::count_if(
            m_state->activeChunkBlases.begin(), m_state->activeChunkBlases.end(),
            [this](const auto &pos) { return m_state->chunkBlases.contains(pos); }
        ));
    }
    return stats;
}

void VulkanRayTracingScene::pollFinishedTlasBuild(
    VulkanContext &context, uint64_t frameCounter, bool forcePoll
) {
    if (!m_state || !m_state->ready) {
        return;
    }

    RtSceneState &rt = *m_state;
    if (!rt.pendingTlasBuild.has_value()) {
        return;
    }
    if (!forcePoll && frameCounter <= rt.pendingTlasBuild->submitFrame) {
        return;
    }

    const std::array<vk::Fence, 1> fences = {*rt.pendingTlasBuild->fence};
    const vk::Result status = context.getDevice().waitForFences(fences, vk::True, 0);
    if (status == vk::Result::eTimeout) {
        return;
    }
    if (status != vk::Result::eSuccess) {
        std::cerr << "[Vulkan][RT] TLAS build fence failed status=" << static_cast<int>(status)
                  << "\n";
        m_highPriorityTlasRefreshRequested =
            m_highPriorityTlasRefreshRequested || rt.pendingTlasBuild->highPriority;
        rt.pendingTlasBuild->reset();
        rt.pendingTlasBuild.reset();
        m_dirty = true;
        return;
    }

    if (rt.pendingTlasBuild->timingQueries != nullptr) {
        const auto &pending = *rt.pendingTlasBuild;
        std::array<uint64_t, 2> timestamps{};
        const VkResult result = vkGetQueryPoolResults(
            static_cast<VkDevice>(*context.getDevice()), static_cast<VkQueryPool>(*pending.timingQueries),
            0, 2, sizeof(timestamps), timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
        );
        if (result == VK_SUCCESS) {
            const uint64_t mask = pending.timestampValidBits >= 64
                                      ? std::numeric_limits<uint64_t>::max()
                                      : (uint64_t{1} << pending.timestampValidBits) - 1u;
            const uint64_t elapsed = (timestamps[1] - timestamps[0]) & mask;
            std::cout << "[Vulkan][Timing][TLAS] frame=" << pending.submitFrame
                      << " queueElapsedMs=" << (static_cast<double>(elapsed) * pending.timestampPeriod / 1e6)
                      << " instances=" << pending.instanceCount << "\n";
        }
    }

    if (rt.tlas != nullptr) {
        vk::raii::Fence completion{nullptr};
        try {
            rt.retiredTlases.reserve(rt.retiredTlases.size() + 1u);
            completion = vk::raii::Fence(context.getDevice(), vk::FenceCreateInfo{});
            const vk::SubmitInfo marker{};
            context.getGraphicsQueue().submit(marker, *completion);
        } catch (const std::exception &e) {
            std::cerr << "[Vulkan][RT] Failed to retire TLAS: " << e.what() << "\n";
            return;
        }
        RtSceneState::RetiredTlas retired{};
        retired.as = std::move(rt.tlas);
        retired.asBuffer = std::move(rt.tlasAsBuffer);
        retired.asCapacity = rt.tlasAsBufferCapacity;
        retired.graphicsCompletion = std::move(completion);
        retired.referencedBlases = std::move(rt.tlasReferencedBlases);
        rt.retiredTlases.push_back(std::move(retired));
        rt.tlasAsBufferCapacity = 0;
    }

    rt.tlas = std::move(rt.pendingTlasBuild->as);
    rt.tlasReferencedBlases = std::move(rt.pendingTlasBuild->referencedBlases);
    rt.tlasAsBuffer = std::move(rt.pendingTlasBuild->asBuffer);
    rt.tlasAsBufferCapacity = rt.pendingTlasBuild->asCapacity;
    m_activeTlas = (rt.tlas != nullptr)
                       ? static_cast<VkAccelerationStructureKHR>(
                             static_cast<vk::AccelerationStructureKHR>(*rt.tlas)
                         )
                       : VK_NULL_HANDLE;
    rt.pendingTlasBuild->reset();
    rt.pendingTlasBuild.reset();
}
