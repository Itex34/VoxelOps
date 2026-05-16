#include "VulkanRenderer.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "../vulkan/VulkanUtils.hpp"

namespace {
    vk::DeviceSize growCapacity(vk::DeviceSize minimum, vk::DeviceSize required) {
        if (required <= minimum) {
            return minimum;
        }

        vk::DeviceSize capacity = minimum;
        while (capacity < required) {
            capacity *= 2;
        }
        return capacity;
    }
} // namespace

void VulkanRenderer::createModelDescriptorResources() {
    cleanupModelDescriptorResources();

    if (m_framebuffers.empty()) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();

    vk::DescriptorSetLayoutBinding storageBinding{};
    storageBinding.binding = 0;
    storageBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
    storageBinding.descriptorCount = 1;
    storageBinding.stageFlags = vk::ShaderStageFlagBits::eVertex;

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &storageBinding;
    m_modelDescriptorSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);

    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eStorageBuffer;
    poolSize.descriptorCount = static_cast<uint32_t>(m_framebuffers.size());

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = static_cast<uint32_t>(m_framebuffers.size());
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    m_modelDescriptorPool = vk::raii::DescriptorPool(device, poolInfo);

    std::vector<vk::DescriptorSetLayout> layouts(
        m_framebuffers.size(), *m_modelDescriptorSetLayout
    );
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *m_modelDescriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    m_modelDescriptorSets = device.allocateDescriptorSets(allocateInfo);

    m_perImageDrawResources.clear();
    m_perImageDrawResources.resize(m_framebuffers.size());
}

void VulkanRenderer::cleanupModelDescriptorResources() {
    m_modelDescriptorSets.clear();
    m_modelDescriptorPool.clear();
    m_modelDescriptorSetLayout.clear();
}

void VulkanRenderer::ensurePerImageDrawBufferCapacity(
    uint32_t imageIndex, vk::DeviceSize modelBytes, vk::DeviceSize indirectBytes
) {
    if (imageIndex >= m_perImageDrawResources.size()) {
        return;
    }

    PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();

    const vk::DeviceSize requiredModelBytes = std::max(modelBytes, MIN_MODEL_BUFFER_BYTES);
    if (resources.modelMatrixCapacityBytes < requiredModelBytes ||
        resources.modelMatrixBuffer == nullptr) {
        if (resources.modelMatrixMapped != nullptr) {
            resources.modelMatrixBufferMemory.unmapMemory();
            resources.modelMatrixMapped = nullptr;
        }
        resources.modelMatrixBuffer.clear();
        resources.modelMatrixBufferMemory.clear();
        resources.modelMatrixCapacityBytes =
            growCapacity(MIN_MODEL_BUFFER_BYTES, requiredModelBytes);

        VulkanUtils::createBuffer(
            device,
            physicalDevice,
            resources.modelMatrixCapacityBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            resources.modelMatrixBuffer,
            resources.modelMatrixBufferMemory
        );
        resources.modelMatrixMapped = resources.modelMatrixBufferMemory.mapMemory(0, VK_WHOLE_SIZE);

        updateModelDescriptorSet(imageIndex);
    }

    const vk::DeviceSize requiredIndirectBytes = std::max(indirectBytes, MIN_INDIRECT_BUFFER_BYTES);
    if (resources.indirectCommandCapacityBytes < requiredIndirectBytes ||
        resources.indirectCommandBuffer == nullptr) {
        if (resources.indirectCommandMapped != nullptr) {
            resources.indirectCommandBufferMemory.unmapMemory();
            resources.indirectCommandMapped = nullptr;
        }
        resources.indirectCommandBuffer.clear();
        resources.indirectCommandBufferMemory.clear();
        resources.indirectCommandCapacityBytes =
            growCapacity(MIN_INDIRECT_BUFFER_BYTES, requiredIndirectBytes);

        VulkanUtils::createBuffer(
            device,
            physicalDevice,
            resources.indirectCommandCapacityBytes,
            vk::BufferUsageFlagBits::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            resources.indirectCommandBuffer,
            resources.indirectCommandBufferMemory
        );
        resources.indirectCommandMapped =
            resources.indirectCommandBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
    }
}

void VulkanRenderer::updatePerImageDrawBuffers(
    uint32_t imageIndex,
    const std::vector<glm::mat4> &modelMatrices,
    const std::vector<IndexedIndirectCommand> &indirectCommands,
    const std::vector<PackedVoxelVertexGpu> &chunkSuperbatchVertices,
    const std::vector<uint16_t> &chunkSuperbatchIndices
) {
    if (imageIndex >= m_perImageDrawResources.size()) {
        return;
    }

    const vk::DeviceSize modelBytes =
        static_cast<vk::DeviceSize>(modelMatrices.size() * sizeof(glm::mat4));
    const vk::DeviceSize indirectBytes =
        static_cast<vk::DeviceSize>(indirectCommands.size() * sizeof(IndexedIndirectCommand));
    ensurePerImageDrawBufferCapacity(imageIndex, modelBytes, indirectBytes);

    PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];

    if (!modelMatrices.empty() && resources.modelMatrixMapped != nullptr) {
        std::memcpy(
            resources.modelMatrixMapped, modelMatrices.data(), static_cast<size_t>(modelBytes)
        );
    }

    if (!indirectCommands.empty() && resources.indirectCommandMapped != nullptr) {
        std::memcpy(
            resources.indirectCommandMapped,
            indirectCommands.data(),
            static_cast<size_t>(indirectBytes)
        );
    }

    const vk::DeviceSize chunkSuperVertexBytes = static_cast<vk::DeviceSize>(
        chunkSuperbatchVertices.size() * sizeof(PackedVoxelVertexGpu)
    );
    const vk::DeviceSize chunkSuperIndexBytes =
        static_cast<vk::DeviceSize>(chunkSuperbatchIndices.size() * sizeof(uint16_t));
    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();

    if (chunkSuperVertexBytes > 0) {
        if (resources.chunkSuperbatchVertexCapacityBytes < chunkSuperVertexBytes ||
            resources.chunkSuperbatchVertexBuffer == nullptr) {
            if (resources.chunkSuperbatchVertexMapped != nullptr) {
                resources.chunkSuperbatchVertexBufferMemory.unmapMemory();
                resources.chunkSuperbatchVertexMapped = nullptr;
            }
            resources.chunkSuperbatchVertexBuffer.clear();
            resources.chunkSuperbatchVertexBufferMemory.clear();
            resources.chunkSuperbatchVertexCapacityBytes =
                growCapacity(4096u, chunkSuperVertexBytes);
            VulkanUtils::createBuffer(
                device,
                physicalDevice,
                resources.chunkSuperbatchVertexCapacityBytes,
                vk::BufferUsageFlagBits::eVertexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                resources.chunkSuperbatchVertexBuffer,
                resources.chunkSuperbatchVertexBufferMemory
            );
            resources.chunkSuperbatchVertexMapped =
                resources.chunkSuperbatchVertexBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
        }
        if (resources.chunkSuperbatchVertexMapped != nullptr) {
            std::memcpy(
                resources.chunkSuperbatchVertexMapped,
                chunkSuperbatchVertices.data(),
                static_cast<size_t>(chunkSuperVertexBytes)
            );
        }
    }

    if (chunkSuperIndexBytes > 0) {
        if (resources.chunkSuperbatchIndexCapacityBytes < chunkSuperIndexBytes ||
            resources.chunkSuperbatchIndexBuffer == nullptr) {
            if (resources.chunkSuperbatchIndexMapped != nullptr) {
                resources.chunkSuperbatchIndexBufferMemory.unmapMemory();
                resources.chunkSuperbatchIndexMapped = nullptr;
            }
            resources.chunkSuperbatchIndexBuffer.clear();
            resources.chunkSuperbatchIndexBufferMemory.clear();
            resources.chunkSuperbatchIndexCapacityBytes =
                growCapacity(4096u, chunkSuperIndexBytes);
            VulkanUtils::createBuffer(
                device,
                physicalDevice,
                resources.chunkSuperbatchIndexCapacityBytes,
                vk::BufferUsageFlagBits::eIndexBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                resources.chunkSuperbatchIndexBuffer,
                resources.chunkSuperbatchIndexBufferMemory
            );
            resources.chunkSuperbatchIndexMapped =
                resources.chunkSuperbatchIndexBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
        }
        if (resources.chunkSuperbatchIndexMapped != nullptr) {
            std::memcpy(
                resources.chunkSuperbatchIndexMapped,
                chunkSuperbatchIndices.data(),
                static_cast<size_t>(chunkSuperIndexBytes)
            );
        }
    }
}

void VulkanRenderer::updateModelDescriptorSet(uint32_t imageIndex) {
    if (imageIndex >= m_perImageDrawResources.size() ||
        imageIndex >= m_modelDescriptorSets.size()) {
        return;
    }

    const PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
    if (resources.modelMatrixBuffer == nullptr || resources.modelMatrixCapacityBytes == 0) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();

    vk::DescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = *resources.modelMatrixBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = resources.modelMatrixCapacityBytes;

    vk::WriteDescriptorSet write{};
    write.dstSet = *m_modelDescriptorSets[imageIndex];
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    const std::array<vk::WriteDescriptorSet, 1> writes = {write};
    device.updateDescriptorSets(writes, {});
}

void VulkanRenderer::cleanupPerImageDrawResources() {
    for (PerImageDrawResources &resources : m_perImageDrawResources) {
        if (resources.modelMatrixMapped != nullptr) {
            resources.modelMatrixBufferMemory.unmapMemory();
            resources.modelMatrixMapped = nullptr;
        }
        resources.modelMatrixBuffer.clear();
        resources.modelMatrixBufferMemory.clear();
        resources.modelMatrixCapacityBytes = 0;

        if (resources.indirectCommandMapped != nullptr) {
            resources.indirectCommandBufferMemory.unmapMemory();
            resources.indirectCommandMapped = nullptr;
        }
        resources.indirectCommandBuffer.clear();
        resources.indirectCommandBufferMemory.clear();
        resources.indirectCommandCapacityBytes = 0;

        if (resources.chunkSuperbatchVertexMapped != nullptr) {
            resources.chunkSuperbatchVertexBufferMemory.unmapMemory();
            resources.chunkSuperbatchVertexMapped = nullptr;
        }
        resources.chunkSuperbatchVertexBuffer.clear();
        resources.chunkSuperbatchVertexBufferMemory.clear();
        resources.chunkSuperbatchVertexCapacityBytes = 0;

        if (resources.chunkSuperbatchIndexMapped != nullptr) {
            resources.chunkSuperbatchIndexBufferMemory.unmapMemory();
            resources.chunkSuperbatchIndexMapped = nullptr;
        }
        resources.chunkSuperbatchIndexBuffer.clear();
        resources.chunkSuperbatchIndexBufferMemory.clear();
        resources.chunkSuperbatchIndexCapacityBytes = 0;

        if (resources.giParamsMapped != nullptr) {
            resources.giParamsBufferMemory.unmapMemory();
            resources.giParamsMapped = nullptr;
        }
        resources.giParamsBuffer.clear();
        resources.giParamsBufferMemory.clear();
    }
    m_perImageDrawResources.clear();
}
