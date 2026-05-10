#include "VulkanRayTracingScene.hpp"

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
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
    constexpr glm::ivec3 kRtDummyChunkPos{250000, 250000, 250000};
    constexpr uint64_t kRtRetireFrameLag = 24u;

    struct DeviceBufferAllocation {
        VmaAllocator allocator = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;

        DeviceBufferAllocation() = default;
        ~DeviceBufferAllocation() {
            reset();
        }
        DeviceBufferAllocation(const DeviceBufferAllocation &) = delete;
        DeviceBufferAllocation &operator=(const DeviceBufferAllocation &) = delete;
        DeviceBufferAllocation(DeviceBufferAllocation &&other) noexcept {
            allocator = other.allocator;
            buffer = other.buffer;
            allocation = other.allocation;
            other.allocator = VK_NULL_HANDLE;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
        }
        DeviceBufferAllocation &operator=(DeviceBufferAllocation &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            allocator = other.allocator;
            buffer = other.buffer;
            allocation = other.allocation;
            other.allocator = VK_NULL_HANDLE;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            return *this;
        }

        void reset() {
            if (allocator != VK_NULL_HANDLE && buffer != VK_NULL_HANDLE &&
                allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, buffer, allocation);
            }
            allocator = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
        }
    };

    void createBufferWithAddressing(
        VmaAllocator allocator,
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags properties,
        bool hostAccessSequentialWrite,
        DeviceBufferAllocation &outBuffer
    ) {
        outBuffer.reset();
        if (allocator == VK_NULL_HANDLE) {
            throw std::runtime_error("VMA allocator is not initialized.");
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = std::max<vk::DeviceSize>(size, 4u);
        bufferInfo.usage = static_cast<VkBufferUsageFlags>(usage);
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(properties);
        if (hostAccessSequentialWrite) {
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        outBuffer.allocator = allocator;
        const VkResult result =
            vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &outBuffer.buffer, &outBuffer.allocation, nullptr);
        if (result != VK_SUCCESS) {
            outBuffer.reset();
            throw std::runtime_error("Failed to create VMA buffer allocation.");
        }
    }

    vk::DeviceAddress getBufferDeviceAddress(const vk::raii::Device &device, vk::Buffer buffer) {
        vk::BufferDeviceAddressInfo addressInfo{};
        addressInfo.buffer = buffer;
        return device.getBufferAddress(addressInfo);
    }

    glm::vec3 decodePackedVoxelPosition(const VoxelVertex &packed) {
        const uint32_t x = (packed.low >> 0u) & 31u;
        const uint32_t y = (packed.low >> 5u) & 31u;
        const uint32_t z = (packed.low >> 10u) & 31u;
        return glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
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
        uint32_t primitiveCount = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            indexBuffer.reset();
            vertexBuffer.reset();
            revision = 0;
            primitiveCount = 0;
        }
    };

    struct RetiredChunkBlas {
        ChunkBlas blas{};
        uint64_t retireFrame = 0;
    };

    struct RetiredTlas {
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        uint64_t retireFrame = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            retireFrame = 0;
        }
    };

    struct PendingGpuChunkBlas {
        glm::ivec3 chunkPos{0};
        uint64_t revision = 0;
        bool highPriority = false;
        ChunkBlas built{};
    };

    struct PendingGpuBuildBatch {
        vk::raii::Fence fence{nullptr};
        vk::raii::CommandBuffer commandBuffer{nullptr};
        std::vector<PendingGpuChunkBlas> chunks;
        std::vector<DeviceBufferAllocation> stagingBuffers;
        uint64_t submitFrame = 0;
    };

    struct PendingGpuTlasBuild {
        vk::raii::Fence fence{nullptr};
        vk::raii::CommandBuffer commandBuffer{nullptr};
        DeviceBufferAllocation instanceBuffer{};
        DeviceBufferAllocation scratchBuffer{};
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        uint64_t submitFrame = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            scratchBuffer.reset();
            instanceBuffer.reset();
            commandBuffer.clear();
            fence.clear();
            submitFrame = 0;
        }
    };

    vk::raii::CommandPool commandPool{nullptr};
    ChunkBlas dummyBlas{};
    std::unordered_map<glm::ivec3, ChunkBlas, IVec3Hash> chunkBlases;
    std::vector<RetiredChunkBlas> retiredChunkBlases;
    DeviceBufferAllocation blasScratchBuffer{};
    vk::DeviceSize blasScratchBufferSize = 0;
    std::vector<PendingGpuBuildBatch> pendingGpuBuildBatches;

    DeviceBufferAllocation tlasAsBuffer{};
    vk::raii::AccelerationStructureKHR tlas{nullptr};
    std::vector<RetiredTlas> retiredTlases;
    std::optional<PendingGpuTlasBuild> pendingTlasBuild;

    bool ready = false;

    void reset() {
        tlas.clear();
        tlasAsBuffer.reset();
        if (pendingTlasBuild.has_value()) {
            pendingTlasBuild->reset();
            pendingTlasBuild.reset();
        }
        for (RetiredTlas &retired : retiredTlases) {
            retired.reset();
        }
        retiredTlases.clear();

        for (RetiredChunkBlas &retired : retiredChunkBlases) {
            retired.blas.reset();
        }
        retiredChunkBlases.clear();

        for (auto &[_, chunkBlas] : chunkBlases) {
            chunkBlas.reset();
        }
        chunkBlases.clear();
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
        poolInfo.queueFamilyIndex = context.getGraphicsQueueFamily();
        rt.commandPool = vk::raii::CommandPool(context.getDevice(), poolInfo);
    }

    rt.ready = true;
    if (rt.chunkBlases.find(kRtDummyChunkPos) == rt.chunkBlases.end()) {
        CpuChunkMesh dummyMesh{};
        dummyMesh.vertices = {
            makePackedVoxelVertex(0u, 0u, 0u),
            makePackedVoxelVertex(1u, 0u, 0u),
            makePackedVoxelVertex(0u, 1u, 0u)
        };
        dummyMesh.indices = {0u, 1u, 2u};
        dummyMesh.revision = 1u;
        (void)uploadChunkGeometry(
            context, uploadContext, frameCounter, kRtDummyChunkPos, dummyMesh
        );
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> bootstrapCpuMeshes = {
            {kRtDummyChunkPos, dummyMesh}
        };
        (void)processPendingUploads(context, uploadContext, frameCounter, 1u, bootstrapCpuMeshes);
        while (!rt.pendingGpuBuildBatches.empty()) {
            const std::array<vk::Fence, 1> fences = {*rt.pendingGpuBuildBatches.front().fence};
            (void)context.getDevice().waitForFences(
                fences, vk::True, std::numeric_limits<uint64_t>::max()
            );
            pollFinishedBlasBuilds(context, frameCounter);
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
            pollFinishedTlasBuild(context, frameCounter);
        }
    }
}

void VulkanRayTracingScene::collectRetiredResources(uint64_t frameCounter) {
    if (!m_state) {
        return;
    }

    RtSceneState &rt = *m_state;
    for (auto it = rt.retiredChunkBlases.begin(); it != rt.retiredChunkBlases.end();) {
        if (it->retireFrame > frameCounter) {
            ++it;
            continue;
        }
        it->blas.reset();
        it = rt.retiredChunkBlases.erase(it);
    }
    for (auto it = rt.retiredTlases.begin(); it != rt.retiredTlases.end();) {
        if (it->retireFrame > frameCounter) {
            ++it;
            continue;
        }
        it->reset();
        it = rt.retiredTlases.erase(it);
    }
}

void VulkanRayTracingScene::reset() {
    m_activeTlas = VK_NULL_HANDLE;
    m_dirty = false;
    m_highPriorityTlasRefreshRequested = false;
    m_highPriorityChunkUploadQueue.clear();
    m_pendingChunkUploadQueue.clear();
    m_pendingChunkUploadSet.clear();
    m_highPriorityChunkUploadPending.clear();
    m_cancelledChunkBuilds.clear();
    m_pendingChunkUploads.clear();
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
    bool highPriority
) {
    (void)context;
    (void)uploadContext;
    (void)frameCounter;
    if (!m_state || !m_state->ready) {
        return false;
    }
    if (cpuMesh.vertices.empty() || cpuMesh.indices.empty()) {
        return false;
    }

    RtSceneState &rt = *m_state;
    auto existingIt = rt.chunkBlases.find(chunkPos);
    if (existingIt != rt.chunkBlases.end() && existingIt->second.revision == cpuMesh.revision) {
        return true;
    }

    auto pendingIt = m_pendingChunkUploads.find(chunkPos);
    if (pendingIt != m_pendingChunkUploads.end() &&
        pendingIt->second.revision == cpuMesh.revision) {
        if (highPriority && !pendingIt->second.highPriority) {
            pendingIt->second.highPriority = true;
            if (m_highPriorityChunkUploadPending.insert(chunkPos).second) {
                m_highPriorityChunkUploadQueue.push_back(chunkPos);
            }
        }
        return true;
    }

    m_cancelledChunkBuilds.erase(chunkPos);
    m_pendingChunkUploads[chunkPos] = PendingChunkUpload{cpuMesh.revision, highPriority};
    if (m_pendingChunkUploadSet.insert(chunkPos).second) {
        if (highPriority) {
            m_highPriorityChunkUploadPending.insert(chunkPos);
            m_highPriorityChunkUploadQueue.push_back(chunkPos);
        } else {
            m_pendingChunkUploadQueue.push_back(chunkPos);
        }
    } else if (highPriority && m_highPriorityChunkUploadPending.insert(chunkPos).second) {
        // Promote an already queued chunk so it is processed ahead of background uploads.
        m_highPriorityChunkUploadQueue.push_back(chunkPos);
    }
    return true;
}

size_t VulkanRayTracingScene::processPendingUploads(
    VulkanContext &context,
    UploadContext &uploadContext,
    uint64_t frameCounter,
    size_t maxUploadsPerCall,
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuChunkMeshes
) {
    (void)uploadContext;
    if (!m_state || !m_state->ready || maxUploadsPerCall == 0) {
        return 0;
    }
    RtSceneState &rt = *m_state;
    if (!rt.pendingGpuBuildBatches.empty()) {
        return 0;
    }

    struct PendingBuild {
        RtSceneState::PendingGpuChunkBlas pending{};
        vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
        vk::AccelerationStructureGeometryKHR geometry{};
        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        std::vector<glm::vec3> positions;
        std::vector<uint16_t> indices;
        vk::DeviceSize vertexBytes = 0;
        vk::DeviceSize indexBytes = 0;
        vk::DeviceSize vertexOffset = 0;
        vk::DeviceSize indexOffset = 0;
    };

    const vk::raii::Device &device = context.getDevice();
    const VmaAllocator vmaAllocator = context.getVmaAllocator();

    std::vector<PendingBuild> pendingBuilds;
    pendingBuilds.reserve(maxUploadsPerCall);
    size_t processed = 0;
    vk::DeviceSize maxScratchSize = 0;

    while (processed < maxUploadsPerCall &&
           (!m_highPriorityChunkUploadQueue.empty() || !m_pendingChunkUploadQueue.empty())) {
        std::optional<glm::ivec3> nextChunkPos;
        bool dequeuedHighPriority = false;
        while (!m_highPriorityChunkUploadQueue.empty()) {
            const glm::ivec3 chunkPos = m_highPriorityChunkUploadQueue.front();
            m_highPriorityChunkUploadQueue.pop_front();
            if (m_highPriorityChunkUploadPending.erase(chunkPos) == 0) {
                continue;
            }
            if (m_pendingChunkUploadSet.erase(chunkPos) == 0) {
                continue;
            }
            nextChunkPos = chunkPos;
            dequeuedHighPriority = true;
            break;
        }
        if (!nextChunkPos.has_value()) {
            while (!m_pendingChunkUploadQueue.empty()) {
                const glm::ivec3 chunkPos = m_pendingChunkUploadQueue.front();
                m_pendingChunkUploadQueue.pop_front();
                if (m_highPriorityChunkUploadPending.find(chunkPos) !=
                    m_highPriorityChunkUploadPending.end()) {
                    continue;
                }
                if (m_pendingChunkUploadSet.erase(chunkPos) == 0) {
                    continue;
                }
                nextChunkPos = chunkPos;
                break;
            }
        }
        if (!nextChunkPos.has_value()) {
            break;
        }
        const glm::ivec3 chunkPos = nextChunkPos.value();

        auto pendingIt = m_pendingChunkUploads.find(chunkPos);
        if (pendingIt == m_pendingChunkUploads.end()) {
            continue;
        }
        PendingChunkUpload pendingUpload = std::move(pendingIt->second);
        m_pendingChunkUploads.erase(pendingIt);
        const bool highPriorityUpload = dequeuedHighPriority || pendingUpload.highPriority;

        auto cpuMeshIt = cpuChunkMeshes.find(chunkPos);
        if (cpuMeshIt == cpuChunkMeshes.end()) {
            ++processed;
            continue;
        }
        const CpuChunkMesh &cpuMesh = cpuMeshIt->second;
        if (cpuMesh.revision != pendingUpload.revision) {
            ++processed;
            continue;
        }

        auto existingIt = rt.chunkBlases.find(chunkPos);
        if (existingIt != rt.chunkBlases.end() && existingIt->second.revision == cpuMesh.revision) {
            ++processed;
            continue;
        }
        if (cpuMesh.vertices.empty() || cpuMesh.indices.empty()) {
            ++processed;
            continue;
        }

        std::vector<glm::vec3> positions;
        positions.reserve(cpuMesh.vertices.size());
        for (const VoxelVertex &packed : cpuMesh.vertices) {
            positions.push_back(decodePackedVoxelPosition(packed));
        }
        if (positions.empty() || cpuMesh.indices.empty()) {
            ++processed;
            continue;
        }

        PendingBuild build{};
        build.pending.chunkPos = chunkPos;
        build.pending.revision = cpuMesh.revision;
        build.pending.highPriority = highPriorityUpload;
        build.positions = std::move(positions);
        build.indices = cpuMesh.indices;
        build.vertexBytes =
            static_cast<vk::DeviceSize>(build.positions.size() * sizeof(glm::vec3));
        build.indexBytes =
            static_cast<vk::DeviceSize>(build.indices.size() * sizeof(uint16_t));

        createBufferWithAddressing(
            vmaAllocator,
            build.vertexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            build.pending.built.vertexBuffer
        );
        createBufferWithAddressing(
            vmaAllocator,
            build.indexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            build.pending.built.indexBuffer
        );

        const vk::DeviceAddress vertexAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(build.pending.built.vertexBuffer.buffer));
        const vk::DeviceAddress indexAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(build.pending.built.indexBuffer.buffer));
        const uint32_t primitiveCount = static_cast<uint32_t>(build.indices.size() / 3u);
        if (primitiveCount == 0u) {
            ++processed;
            continue;
        }

        build.triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
        build.triangles.vertexData.deviceAddress = vertexAddress;
        build.triangles.vertexStride = sizeof(glm::vec3);
        build.triangles.maxVertex = static_cast<uint32_t>(positions.size() - 1u);
        build.triangles.indexType = vk::IndexType::eUint16;
        build.triangles.indexData.deviceAddress = indexAddress;

        build.geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
        build.geometry.flags = vk::GeometryFlagBitsKHR::eOpaque;
        build.geometry.geometry.triangles = build.triangles;

        build.buildInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        build.buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
        build.buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        build.buildInfo.geometryCount = 1;
        build.buildInfo.pGeometries = &build.geometry;

        const std::array<uint32_t, 1> primitiveCounts = {primitiveCount};
        const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo =
            device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, build.buildInfo, primitiveCounts
            );
        maxScratchSize = std::max(maxScratchSize, sizeInfo.buildScratchSize);

        createBufferWithAddressing(
            vmaAllocator,
            sizeInfo.accelerationStructureSize,
            vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            build.pending.built.asBuffer
        );
        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = static_cast<vk::Buffer>(build.pending.built.asBuffer.buffer);
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        build.pending.built.as = vk::raii::AccelerationStructureKHR(device, asCreateInfo);
        build.pending.built.revision = build.pending.revision;
        build.pending.built.primitiveCount = primitiveCount;
        build.rangeInfo.primitiveCount = primitiveCount;

        pendingBuilds.push_back(std::move(build));
        pendingBuilds.back().buildInfo.pGeometries = &pendingBuilds.back().geometry;
        ++processed;
    }

    if (pendingBuilds.empty()) {
        return processed;
    }

    DeviceBufferAllocation batchVertexStaging{};
    DeviceBufferAllocation batchIndexStaging{};
    vk::DeviceSize totalVertexBytes = 0;
    vk::DeviceSize totalIndexBytes = 0;
    for (PendingBuild &build : pendingBuilds) {
        build.vertexOffset = totalVertexBytes;
        build.indexOffset = totalIndexBytes;
        totalVertexBytes += build.vertexBytes;
        totalIndexBytes += build.indexBytes;
    }
    if (totalVertexBytes > 0) {
        createBufferWithAddressing(
            vmaAllocator,
            totalVertexBytes,
            vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            true,
            batchVertexStaging
        );
        void *mapped = nullptr;
        const VkResult mapResult =
            vmaMapMemory(vmaAllocator, batchVertexStaging.allocation, &mapped);
        if (mapResult != VK_SUCCESS || mapped == nullptr) {
            throw std::runtime_error("Failed to map batched vertex staging buffer.");
        }
        uint8_t *cursor = static_cast<uint8_t *>(mapped);
        for (const PendingBuild &build : pendingBuilds) {
            if (build.vertexBytes == 0) {
                continue;
            }
            std::memcpy(cursor + static_cast<size_t>(build.vertexOffset), build.positions.data(), static_cast<size_t>(build.vertexBytes));
        }
        vmaUnmapMemory(vmaAllocator, batchVertexStaging.allocation);
    }
    if (totalIndexBytes > 0) {
        createBufferWithAddressing(
            vmaAllocator,
            totalIndexBytes,
            vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            true,
            batchIndexStaging
        );
        void *mapped = nullptr;
        const VkResult mapResult =
            vmaMapMemory(vmaAllocator, batchIndexStaging.allocation, &mapped);
        if (mapResult != VK_SUCCESS || mapped == nullptr) {
            throw std::runtime_error("Failed to map batched index staging buffer.");
        }
        uint8_t *cursor = static_cast<uint8_t *>(mapped);
        for (const PendingBuild &build : pendingBuilds) {
            if (build.indexBytes == 0) {
                continue;
            }
            std::memcpy(cursor + static_cast<size_t>(build.indexOffset), build.indices.data(), static_cast<size_t>(build.indexBytes));
        }
        vmaUnmapMemory(vmaAllocator, batchIndexStaging.allocation);
    }

    if (maxScratchSize > rt.blasScratchBufferSize || rt.blasScratchBuffer.buffer == VK_NULL_HANDLE) {
        rt.blasScratchBuffer.reset();
        createBufferWithAddressing(
            vmaAllocator,
            std::max<vk::DeviceSize>(maxScratchSize, 4u),
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            rt.blasScratchBuffer
        );
        rt.blasScratchBufferSize = std::max<vk::DeviceSize>(maxScratchSize, 4u);
    }

    try {
        const vk::DeviceAddress scratchAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(rt.blasScratchBuffer.buffer));
        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
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
                static_cast<vk::Buffer>(batchVertexStaging.buffer),
                static_cast<vk::Buffer>(build.pending.built.vertexBuffer.buffer),
                vertexRegions
            );

            vk::BufferCopy indexCopy{};
            indexCopy.srcOffset = build.indexOffset;
            indexCopy.size = build.indexBytes;
            const std::array<vk::BufferCopy, 1> indexRegions = {indexCopy};
            commandBuffer.copyBuffer(
                static_cast<vk::Buffer>(batchIndexStaging.buffer),
                static_cast<vk::Buffer>(build.pending.built.indexBuffer.buffer),
                indexRegions
            );

            vk::BufferMemoryBarrier vertexBarrier{};
            vertexBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            vertexBarrier.dstAccessMask = vk::AccessFlagBits::eAccelerationStructureReadKHR;
            vertexBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vertexBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vertexBarrier.buffer = static_cast<vk::Buffer>(build.pending.built.vertexBuffer.buffer);
            vertexBarrier.offset = 0;
            vertexBarrier.size = VK_WHOLE_SIZE;
            vk::BufferMemoryBarrier indexBarrier{};
            indexBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            indexBarrier.dstAccessMask = vk::AccessFlagBits::eAccelerationStructureReadKHR;
            indexBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            indexBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            indexBarrier.buffer = static_cast<vk::Buffer>(build.pending.built.indexBuffer.buffer);
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
        commandBuffer.end();
        RtSceneState::PendingGpuBuildBatch batch{};
        batch.fence = vk::raii::Fence(device, vk::FenceCreateInfo{});
        batch.commandBuffer = std::move(commandBuffer);
        batch.submitFrame = frameCounter;
        batch.chunks.reserve(pendingBuilds.size());
        batch.stagingBuffers.reserve(2u);
        for (PendingBuild &build : pendingBuilds) {
            batch.chunks.push_back(std::move(build.pending));
        }
        batch.stagingBuffers.push_back(std::move(batchVertexStaging));
        batch.stagingBuffers.push_back(std::move(batchIndexStaging));

        const vk::CommandBuffer rawCommandBuffer = *batch.commandBuffer;
        vk::SubmitInfo submitInfo{};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &rawCommandBuffer;
        context.getGraphicsQueue().submit(submitInfo, *batch.fence);
        rt.pendingGpuBuildBatches.push_back(std::move(batch));
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to build BLAS batch: " << e.what() << "\n";
        return processed;
    }
    return processed;
}

void VulkanRayTracingScene::pollFinishedBlasBuilds(VulkanContext &context, uint64_t frameCounter) {
    if (!m_state || !m_state->ready) {
        return;
    }

    RtSceneState &rt = *m_state;
    const vk::raii::Device &device = context.getDevice();
    for (auto it = rt.pendingGpuBuildBatches.begin(); it != rt.pendingGpuBuildBatches.end();) {
        const std::array<vk::Fence, 1> fences = {*it->fence};
        const vk::Result status = device.waitForFences(fences, vk::True, 0);
        if (status == vk::Result::eTimeout) {
            ++it;
            continue;
        }
        if (status != vk::Result::eSuccess) {
            std::cerr << "[Vulkan][RT] BLAS batch fence failed status="
                      << static_cast<int>(status) << "\n";
            it = rt.pendingGpuBuildBatches.erase(it);
            continue;
        }

        bool sceneChanged = false;
        bool highPrioritySceneChanged = false;
        for (auto &chunkBuild : it->chunks) {
            if (m_cancelledChunkBuilds.find(chunkBuild.chunkPos) != m_cancelledChunkBuilds.end()) {
                m_cancelledChunkBuilds.erase(chunkBuild.chunkPos);
                continue;
            }
            const auto pendingIt = m_pendingChunkUploads.find(chunkBuild.chunkPos);
            if (pendingIt != m_pendingChunkUploads.end() &&
                pendingIt->second.revision > chunkBuild.revision) {
                continue;
            }

            auto existingIt = rt.chunkBlases.find(chunkBuild.chunkPos);
            if (existingIt != rt.chunkBlases.end()) {
                RtSceneState::RetiredChunkBlas retired{};
                retired.blas = std::move(existingIt->second);
                retired.retireFrame = frameCounter + kRtRetireFrameLag;
                rt.retiredChunkBlases.push_back(std::move(retired));
                rt.chunkBlases.erase(existingIt);
            }
            rt.chunkBlases.insert_or_assign(chunkBuild.chunkPos, std::move(chunkBuild.built));
            sceneChanged = true;
            highPrioritySceneChanged = highPrioritySceneChanged || chunkBuild.highPriority;
        }
        if (sceneChanged) {
            m_dirty = true;
            if (highPrioritySceneChanged) {
                m_highPriorityTlasRefreshRequested = true;
            }
        }
        it = rt.pendingGpuBuildBatches.erase(it);
    }
}

void VulkanRayTracingScene::removeChunkGeometry(uint64_t frameCounter, const glm::ivec3 &chunkPos) {
    if (!m_state) {
        return;
    }

    m_pendingChunkUploads.erase(chunkPos);
    m_pendingChunkUploadSet.erase(chunkPos);
    m_highPriorityChunkUploadPending.erase(chunkPos);
    m_cancelledChunkBuilds.insert(chunkPos);

    RtSceneState &rt = *m_state;
    auto it = rt.chunkBlases.find(chunkPos);
    if (it == rt.chunkBlases.end()) {
        return;
    }

    RtSceneState::RetiredChunkBlas retired{};
    retired.blas = std::move(it->second);
    retired.retireFrame = frameCounter + kRtRetireFrameLag;
    rt.retiredChunkBlases.push_back(std::move(retired));
    rt.chunkBlases.erase(it);
    m_dirty = true;
}

bool VulkanRayTracingScene::hasPendingBuildWork() const noexcept {
    if (!m_state || !m_state->ready) {
        return !m_highPriorityChunkUploadQueue.empty() || !m_pendingChunkUploadQueue.empty() ||
               !m_pendingChunkUploads.empty();
    }
    return !m_highPriorityChunkUploadQueue.empty() || !m_pendingChunkUploadQueue.empty() ||
           !m_pendingChunkUploads.empty() ||
           !m_state->pendingGpuBuildBatches.empty() || m_state->pendingTlasBuild.has_value();
}

bool VulkanRayTracingScene::hasHighPriorityBuildWork() const noexcept {
    if (!m_highPriorityChunkUploadQueue.empty() || !m_highPriorityChunkUploadPending.empty()) {
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
    if (!m_state || !m_state->ready) {
        m_activeTlas = VK_NULL_HANDLE;
        m_dirty = false;
        return false;
    }

    try {
        RtSceneState &rt = *m_state;
        const vk::raii::Device &device = context.getDevice();
        const VmaAllocator vmaAllocator = context.getVmaAllocator();

        if (rt.pendingTlasBuild.has_value()) {
            return false;
        }

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(rt.chunkBlases.size());
        for (const auto &[chunkPos, chunkBlas] : rt.chunkBlases) {
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
        }

        if (instances.empty()) {
            m_dirty = false;
            return false;
        }

        RtSceneState::PendingGpuTlasBuild pending{};
        const vk::DeviceSize instanceBytes = static_cast<vk::DeviceSize>(
            std::max<size_t>(1u, instances.size()) * sizeof(VkAccelerationStructureInstanceKHR)
        );
        createBufferWithAddressing(
            vmaAllocator,
            instanceBytes,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            true,
            pending.instanceBuffer
        );
        if (!instances.empty()) {
            void *mapped = nullptr;
            const VkResult mapResult =
                vmaMapMemory(vmaAllocator, pending.instanceBuffer.allocation, &mapped);
            if (mapResult != VK_SUCCESS || mapped == nullptr) {
                throw std::runtime_error("Failed to map TLAS instance buffer.");
            }
            std::memcpy(
                mapped,
                instances.data(),
                static_cast<size_t>(instances.size() * sizeof(VkAccelerationStructureInstanceKHR))
            );
            vmaUnmapMemory(vmaAllocator, pending.instanceBuffer.allocation);
        }

        vk::AccelerationStructureGeometryInstancesDataKHR instancesData{};
        instancesData.arrayOfPointers = VK_FALSE;
        instancesData.data.deviceAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(pending.instanceBuffer.buffer));

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

        createBufferWithAddressing(
            vmaAllocator,
            sizeInfo.accelerationStructureSize,
            vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            pending.asBuffer
        );

        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = static_cast<vk::Buffer>(pending.asBuffer.buffer);
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        pending.as = vk::raii::AccelerationStructureKHR(device, asCreateInfo);

        createBufferWithAddressing(
            vmaAllocator,
            sizeInfo.buildScratchSize,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            false,
            pending.scratchBuffer
        );

        buildInfo.dstAccelerationStructure = *pending.as;
        buildInfo.scratchData.deviceAddress =
            getBufferDeviceAddress(device, static_cast<vk::Buffer>(pending.scratchBuffer.buffer));

        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {&rangeInfo};
        const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {buildInfo};

        pending.commandBuffer = VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        pending.commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
        pending.commandBuffer.end();
        pending.fence = vk::raii::Fence(device, vk::FenceCreateInfo{});
        const vk::CommandBuffer rawCommandBuffer = *pending.commandBuffer;
        vk::SubmitInfo submitInfo{};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &rawCommandBuffer;
        context.getGraphicsQueue().submit(submitInfo, *pending.fence);
        pending.submitFrame = frameCounter;
        rt.pendingTlasBuild = std::move(pending);
        m_highPriorityTlasRefreshRequested = false;
        m_dirty = false;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to rebuild TLAS: " << e.what() << "\n";
        return false;
    }
}

void VulkanRayTracingScene::pollFinishedTlasBuild(VulkanContext &context, uint64_t frameCounter) {
    if (!m_state || !m_state->ready) {
        return;
    }

    RtSceneState &rt = *m_state;
    if (!rt.pendingTlasBuild.has_value()) {
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
        rt.pendingTlasBuild->reset();
        rt.pendingTlasBuild.reset();
        return;
    }

    if (rt.tlas != nullptr) {
        RtSceneState::RetiredTlas retired{};
        retired.as = std::move(rt.tlas);
        retired.asBuffer = std::move(rt.tlasAsBuffer);
        retired.retireFrame = frameCounter + kRtRetireFrameLag;
        rt.retiredTlases.push_back(std::move(retired));
    }

    rt.tlas = std::move(rt.pendingTlasBuild->as);
    rt.tlasAsBuffer = std::move(rt.pendingTlasBuild->asBuffer);
    m_activeTlas = (rt.tlas != nullptr)
                       ? static_cast<VkAccelerationStructureKHR>(
                             static_cast<vk::AccelerationStructureKHR>(*rt.tlas)
                         )
                       : VK_NULL_HANDLE;
    rt.pendingTlasBuild->reset();
    rt.pendingTlasBuild.reset();
}
