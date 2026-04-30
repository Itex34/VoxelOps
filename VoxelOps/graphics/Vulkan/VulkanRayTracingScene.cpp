#include "VulkanRayTracingScene.hpp"

#include "../Mesh.hpp"
#include "../../voxels/Chunk.hpp"
#include "../../world/ChunkManager.hpp"
#include "vulkan/UploadContext.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
constexpr glm::ivec3 kRtDummyChunkPos{250000, 250000, 250000};
constexpr uint64_t kRtRetireFrameLag = 24u;

struct DeviceBufferAllocation {
    vk::raii::Buffer buffer{nullptr};
    vk::raii::DeviceMemory memory{nullptr};

    void reset() {
        buffer.clear();
        memory.clear();
    }
};

void createBufferWithAddressing(const vk::raii::Device &device,
                                const vk::raii::PhysicalDevice &physicalDevice, vk::DeviceSize size,
                                vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties,
                                bool enableDeviceAddress, DeviceBufferAllocation &outBuffer) {
    outBuffer.reset();

    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = std::max<vk::DeviceSize>(size, 4u);
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    outBuffer.buffer = vk::raii::Buffer(device, bufferInfo);

    const vk::MemoryRequirements requirements = outBuffer.buffer.getMemoryRequirements();

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex =
        VulkanUtils::findMemoryType(physicalDevice, requirements.memoryTypeBits, properties);

    vk::MemoryAllocateFlagsInfo allocFlags{};
    if (enableDeviceAddress) {
        allocFlags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
        allocInfo.pNext = &allocFlags;
    }

    outBuffer.memory = vk::raii::DeviceMemory(device, allocInfo);
    outBuffer.buffer.bindMemory(*outBuffer.memory, 0);
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

    vk::raii::CommandPool commandPool{nullptr};
    ChunkBlas dummyBlas{};
    std::unordered_map<glm::ivec3, ChunkBlas, IVec3Hash> chunkBlases;
    std::vector<RetiredChunkBlas> retiredChunkBlases;

    DeviceBufferAllocation tlasAsBuffer{};
    vk::raii::AccelerationStructureKHR tlas{nullptr};
    std::vector<RetiredTlas> retiredTlases;

    bool ready = false;

    void reset() {
        tlas.clear();
        tlasAsBuffer.reset();
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

void VulkanRayTracingScene::initialize(VulkanContext &context, UploadContext &uploadContext,
                                       uint64_t frameCounter) {
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
        dummyMesh.vertices = {makePackedVoxelVertex(0u, 0u, 0u), makePackedVoxelVertex(1u, 0u, 0u),
                              makePackedVoxelVertex(0u, 1u, 0u)};
        dummyMesh.indices = {0u, 1u, 2u};
        dummyMesh.revision = 1u;
        (void)uploadChunkGeometry(context, uploadContext, frameCounter, kRtDummyChunkPos, dummyMesh);
    }
    m_dirty = true;
    (void)rebuild(context, frameCounter);
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
    if (!m_state) {
        return;
    }
    m_state->reset();
    m_state.reset();
}

bool VulkanRayTracingScene::uploadChunkGeometry(VulkanContext &context, UploadContext &uploadContext,
                                                uint64_t frameCounter, const glm::ivec3 &chunkPos,
                                                const CpuChunkMesh &cpuMesh) {
    if (!m_state || !m_state->ready) {
        return false;
    }
    if (cpuMesh.vertices.empty() || cpuMesh.indices.empty()) {
        return false;
    }

    try {
        RtSceneState &rt = *m_state;
        auto existingIt = rt.chunkBlases.find(chunkPos);
        if (existingIt != rt.chunkBlases.end() && existingIt->second.revision == cpuMesh.revision) {
            return true;
        }

        const vk::raii::Device &device = context.getDevice();
        const vk::raii::PhysicalDevice &physicalDevice = context.getPhysicalDevice();

        std::vector<glm::vec3> positions;
        positions.reserve(cpuMesh.vertices.size());
        for (const VoxelVertex &packed : cpuMesh.vertices) {
            positions.push_back(decodePackedVoxelPosition(packed));
        }

        std::vector<uint16_t> indices = cpuMesh.indices;
        if (positions.empty() || indices.empty()) {
            return false;
        }

        RtSceneState::ChunkBlas built{};
        const vk::DeviceSize vertexBytes =
            static_cast<vk::DeviceSize>(positions.size() * sizeof(glm::vec3));
        const vk::DeviceSize indexBytes =
            static_cast<vk::DeviceSize>(indices.size() * sizeof(uint16_t));
        createBufferWithAddressing(
            device, physicalDevice, vertexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            vk::MemoryPropertyFlagBits::eDeviceLocal, true, built.vertexBuffer);
        createBufferWithAddressing(
            device, physicalDevice, indexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            vk::MemoryPropertyFlagBits::eDeviceLocal, true, built.indexBuffer);

        std::vector<UploadContext::BufferCopyUpload> uploads;
        uploads.reserve(2);
        uploads.push_back(UploadContext::BufferCopyUpload{
            uploadContext.createStagingBuffer(physicalDevice, positions.data(), vertexBytes),
            *built.vertexBuffer.buffer, vertexBytes});
        uploads.push_back(UploadContext::BufferCopyUpload{
            uploadContext.createStagingBuffer(physicalDevice, indices.data(), indexBytes),
            *built.indexBuffer.buffer, indexBytes});
        uploadContext.submitCopyBufferBatch(std::move(uploads));
        uploadContext.waitIdle();

        const vk::DeviceAddress vertexAddress = getBufferDeviceAddress(device, *built.vertexBuffer.buffer);
        const vk::DeviceAddress indexAddress = getBufferDeviceAddress(device, *built.indexBuffer.buffer);
        const uint32_t primitiveCount = static_cast<uint32_t>(indices.size() / 3u);
        if (primitiveCount == 0u) {
            built.reset();
            return false;
        }

        vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
        triangles.vertexData.deviceAddress = vertexAddress;
        triangles.vertexStride = sizeof(glm::vec3);
        triangles.maxVertex = static_cast<uint32_t>(positions.size() - 1u);
        triangles.indexType = vk::IndexType::eUint16;
        triangles.indexData.deviceAddress = indexAddress;

        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
        geometry.flags = vk::GeometryFlagBitsKHR::eOpaque;
        geometry.geometry.triangles = triangles;

        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        const std::array<uint32_t, 1> primitiveCounts = {primitiveCount};
        const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo =
            device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, primitiveCounts);

        createBufferWithAddressing(device, physicalDevice, sizeInfo.accelerationStructureSize,
                                   vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, built.asBuffer);

        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = *built.asBuffer.buffer;
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        built.as = vk::raii::AccelerationStructureKHR(device, asCreateInfo);

        DeviceBufferAllocation scratchBuffer{};
        createBufferWithAddressing(device, physicalDevice, sizeInfo.buildScratchSize,
                                   vk::BufferUsageFlagBits::eStorageBuffer |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, scratchBuffer);
        buildInfo.dstAccelerationStructure = *built.as;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device, *scratchBuffer.buffer);

        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {&rangeInfo};
        const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {buildInfo};

        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
        VulkanUtils::endSingleTimeCommands(device, context.getGraphicsQueue(), std::move(commandBuffer));
        scratchBuffer.reset();

        built.revision = cpuMesh.revision;
        built.primitiveCount = primitiveCount;

        if (existingIt != rt.chunkBlases.end()) {
            RtSceneState::RetiredChunkBlas retired{};
            retired.blas = std::move(existingIt->second);
            retired.retireFrame = frameCounter + kRtRetireFrameLag;
            rt.retiredChunkBlases.push_back(std::move(retired));
            rt.chunkBlases.erase(existingIt);
        }

        rt.chunkBlases.insert_or_assign(chunkPos, std::move(built));
        m_dirty = true;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to upload BLAS for chunk (" << chunkPos.x << ", "
                  << chunkPos.y << ", " << chunkPos.z << "): " << e.what() << "\n";
        return false;
    }
}

void VulkanRayTracingScene::removeChunkGeometry(uint64_t frameCounter, const glm::ivec3 &chunkPos) {
    if (!m_state) {
        return;
    }

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

bool VulkanRayTracingScene::rebuild(VulkanContext &context, uint64_t frameCounter) {
    if (!m_state || !m_state->ready) {
        m_activeTlas = VK_NULL_HANDLE;
        m_dirty = false;
        return false;
    }

    try {
        RtSceneState &rt = *m_state;
        const vk::raii::Device &device = context.getDevice();
        const vk::raii::PhysicalDevice &physicalDevice = context.getPhysicalDevice();

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(rt.chunkBlases.size());
        for (const auto &[chunkPos, chunkBlas] : rt.chunkBlases) {
            if (chunkBlas.as == nullptr || chunkBlas.primitiveCount == 0u) {
                continue;
            }

            vk::AccelerationStructureDeviceAddressInfoKHR addrInfo{};
            addrInfo.accelerationStructure = *chunkBlas.as;
            const vk::DeviceAddress blasAddress = device.getAccelerationStructureAddressKHR(addrInfo);
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

        DeviceBufferAllocation instanceBuffer{};
        const vk::DeviceSize instanceBytes = static_cast<vk::DeviceSize>(
            std::max<size_t>(1u, instances.size()) * sizeof(VkAccelerationStructureInstanceKHR));
        createBufferWithAddressing(
            device, physicalDevice, instanceBytes,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, true,
            instanceBuffer);
        if (!instances.empty()) {
            void *mapped = instanceBuffer.memory.mapMemory(0, instanceBytes);
            std::memcpy(
                mapped, instances.data(),
                static_cast<size_t>(instances.size() * sizeof(VkAccelerationStructureInstanceKHR)));
            instanceBuffer.memory.unmapMemory();
        }

        vk::AccelerationStructureGeometryInstancesDataKHR instancesData{};
        instancesData.arrayOfPointers = VK_FALSE;
        instancesData.data.deviceAddress = getBufferDeviceAddress(device, *instanceBuffer.buffer);

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
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, primitiveCounts);

        DeviceBufferAllocation newTlasBuffer{};
        createBufferWithAddressing(device, physicalDevice, sizeInfo.accelerationStructureSize,
                                   vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, newTlasBuffer);

        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = *newTlasBuffer.buffer;
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        vk::raii::AccelerationStructureKHR newTlas(device, asCreateInfo);

        DeviceBufferAllocation scratchBuffer{};
        createBufferWithAddressing(device, physicalDevice, sizeInfo.buildScratchSize,
                                   vk::BufferUsageFlagBits::eStorageBuffer |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, scratchBuffer);

        buildInfo.dstAccelerationStructure = *newTlas;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device, *scratchBuffer.buffer);

        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {&rangeInfo};
        const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {buildInfo};

        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
        VulkanUtils::endSingleTimeCommands(device, context.getGraphicsQueue(), std::move(commandBuffer));
        scratchBuffer.reset();
        instanceBuffer.reset();

        if (rt.tlas != nullptr) {
            RtSceneState::RetiredTlas retired{};
            retired.as = std::move(rt.tlas);
            retired.asBuffer = std::move(rt.tlasAsBuffer);
            retired.retireFrame = frameCounter + kRtRetireFrameLag;
            rt.retiredTlases.push_back(std::move(retired));
        }

        rt.tlas = std::move(newTlas);
        rt.tlasAsBuffer = std::move(newTlasBuffer);
        m_activeTlas = (rt.tlas != nullptr)
                           ? static_cast<VkAccelerationStructureKHR>(
                                 static_cast<vk::AccelerationStructureKHR>(*rt.tlas))
                           : VK_NULL_HANDLE;
        m_dirty = false;
        return m_activeTlas != VK_NULL_HANDLE;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to rebuild TLAS: " << e.what() << "\n";
        return false;
    }
}
