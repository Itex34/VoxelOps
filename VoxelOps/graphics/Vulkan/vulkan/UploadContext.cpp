#include "graphics/Vulkan/vulkan/UploadContext.hpp"

#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

void UploadContext::init(const vk::raii::Device &device, uint32_t queueFamilyIndex,
                         const vk::raii::Queue &queue) {
    cleanup();

    m_device = &device;
    m_queue = &queue;

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    m_commandPool = vk::raii::CommandPool(device, poolInfo);
}

void UploadContext::cleanup() {
    if (m_device && !m_pendingUploads.empty()) {
        waitIdle();
    }

    m_pendingUploads.clear();
    m_commandPool.clear();
    m_queue = nullptr;
    m_device = nullptr;
}

void UploadContext::poll() {
    if (!m_device) {
        return;
    }

    size_t index = 0;
    while (index < m_pendingUploads.size()) {
        const std::array<vk::Fence, 1> fences = {*m_pendingUploads[index].fence};
        const vk::Result waitResult = m_device->waitForFences(fences, vk::True, 0);
        if (waitResult == vk::Result::eSuccess) {
            m_pendingUploads.erase(m_pendingUploads.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (waitResult != vk::Result::eTimeout) {
            throw std::runtime_error("UploadContext::poll failed while waiting for upload fence.");
        }
        ++index;
    }
}

void UploadContext::waitIdle() {
    if (!m_device) {
        return;
    }

    for (auto &upload : m_pendingUploads) {
        const std::array<vk::Fence, 1> fences = {*upload.fence};
        const vk::Result waitResult =
            m_device->waitForFences(fences, vk::True, std::numeric_limits<uint64_t>::max());
        if (waitResult != vk::Result::eSuccess) {
            throw std::runtime_error(
                "UploadContext::waitIdle failed while waiting for upload fence.");
        }
    }

    m_pendingUploads.clear();
}

UploadContext::StagingBuffer
UploadContext::createStagingBuffer(const vk::raii::PhysicalDevice &physicalDevice, const void *data,
                                   vk::DeviceSize size) {
    if (!m_device) {
        throw std::runtime_error("UploadContext::createStagingBuffer called before init.");
    }

    StagingBuffer stagingBuffer{};
    VulkanUtils::createBuffer(
        *m_device, physicalDevice, size, vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        stagingBuffer.buffer, stagingBuffer.memory);

    if (data && size > 0) {
        void *mapped = stagingBuffer.memory.mapMemory(0, size);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        stagingBuffer.memory.unmapMemory();
    }

    return stagingBuffer;
}

void UploadContext::submitCopyBuffer(StagingBuffer &&stagingBuffer, vk::Buffer dstBuffer,
                                     vk::DeviceSize size) {
    if (!m_device || !m_queue) {
        throw std::runtime_error("UploadContext::submitCopyBuffer called before init.");
    }

    PendingUpload pendingUpload{};
    pendingUpload.fence = vk::raii::Fence(*m_device, vk::FenceCreateInfo{});
    pendingUpload.commandBuffer = beginCommandBuffer();
    pendingUpload.stagingBuffers.emplace_back(std::move(stagingBuffer));

    vk::BufferCopy copyRegion{};
    copyRegion.size = size;
    const std::array<vk::BufferCopy, 1> copyRegions = {copyRegion};
    pendingUpload.commandBuffer.copyBuffer(*pendingUpload.stagingBuffers[0].buffer, dstBuffer,
                                           copyRegions);

    submitPendingUpload(std::move(pendingUpload));
}

void UploadContext::submitCopyBufferBatch(std::vector<BufferCopyUpload> &&uploads) {
    if (!m_device || !m_queue) {
        throw std::runtime_error("UploadContext::submitCopyBufferBatch called before init.");
    }
    if (uploads.empty()) {
        return;
    }

    PendingUpload pendingUpload{};
    pendingUpload.stagingBuffers.reserve(uploads.size());
    bool hasCommands = false;

    for (BufferCopyUpload &upload : uploads) {
        if (upload.dstBuffer == VK_NULL_HANDLE || upload.size == 0 ||
            upload.stagingBuffer.buffer == nullptr) {
            continue;
        }

        if (!hasCommands) {
            pendingUpload.fence = vk::raii::Fence(*m_device, vk::FenceCreateInfo{});
            pendingUpload.commandBuffer = beginCommandBuffer();
            hasCommands = true;
        }

        pendingUpload.stagingBuffers.emplace_back(std::move(upload.stagingBuffer));
        StagingBuffer &staging = pendingUpload.stagingBuffers.back();

        vk::BufferCopy copyRegion{};
        copyRegion.size = upload.size;
        const std::array<vk::BufferCopy, 1> copyRegions = {copyRegion};
        pendingUpload.commandBuffer.copyBuffer(*staging.buffer, upload.dstBuffer, copyRegions);
    }

    if (!hasCommands || pendingUpload.stagingBuffers.empty()) {
        return;
    }

    submitPendingUpload(std::move(pendingUpload));
}

void UploadContext::submitImageUpload(StagingBuffer &&stagingBuffer, vk::Image image,
                                      uint32_t width, uint32_t height) {
    submitImageUploadArray(std::move(stagingBuffer), image, width, height, 1);
}

void UploadContext::submitImageUploadArray(StagingBuffer &&stagingBuffer, vk::Image image,
                                           uint32_t width, uint32_t height, uint32_t layerCount) {
    if (!m_device || !m_queue) {
        throw std::runtime_error("UploadContext::submitImageUploadArray called before init.");
    }
    if (layerCount == 0) {
        throw std::runtime_error("UploadContext::submitImageUploadArray called with layerCount=0.");
    }

    PendingUpload pendingUpload{};
    pendingUpload.fence = vk::raii::Fence(*m_device, vk::FenceCreateInfo{});
    pendingUpload.commandBuffer = beginCommandBuffer();
    pendingUpload.stagingBuffers.emplace_back(std::move(stagingBuffer));

    vk::ImageMemoryBarrier toTransferDstBarrier{};
    toTransferDstBarrier.oldLayout = vk::ImageLayout::eUndefined;
    toTransferDstBarrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    toTransferDstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDstBarrier.image = image;
    toTransferDstBarrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    toTransferDstBarrier.subresourceRange.baseMipLevel = 0;
    toTransferDstBarrier.subresourceRange.levelCount = 1;
    toTransferDstBarrier.subresourceRange.baseArrayLayer = 0;
    toTransferDstBarrier.subresourceRange.layerCount = layerCount;
    toTransferDstBarrier.srcAccessMask = {};
    toTransferDstBarrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

    const std::array<vk::ImageMemoryBarrier, 1> toTransferDstBarriers = {toTransferDstBarrier};
    pendingUpload.commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                                vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                                                toTransferDstBarriers);

    const vk::DeviceSize bytesPerLayer =
        static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * 4u;
    std::vector<vk::BufferImageCopy> copyRegions;
    copyRegions.reserve(layerCount);
    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        vk::BufferImageCopy copyRegion{};
        copyRegion.bufferOffset = bytesPerLayer * static_cast<vk::DeviceSize>(layer);
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = layer;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = vk::Offset3D{0, 0, 0};
        copyRegion.imageExtent = vk::Extent3D{width, height, 1};
        copyRegions.push_back(copyRegion);
    }

    pendingUpload.commandBuffer.copyBufferToImage(*pendingUpload.stagingBuffers[0].buffer, image,
                                                  vk::ImageLayout::eTransferDstOptimal,
                                                  copyRegions);

    vk::ImageMemoryBarrier toShaderReadBarrier{};
    toShaderReadBarrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    toShaderReadBarrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    toShaderReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderReadBarrier.image = image;
    toShaderReadBarrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    toShaderReadBarrier.subresourceRange.baseMipLevel = 0;
    toShaderReadBarrier.subresourceRange.levelCount = 1;
    toShaderReadBarrier.subresourceRange.baseArrayLayer = 0;
    toShaderReadBarrier.subresourceRange.layerCount = layerCount;
    toShaderReadBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    toShaderReadBarrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

    const std::array<vk::ImageMemoryBarrier, 1> toShaderReadBarriers = {toShaderReadBarrier};
    pendingUpload.commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                                vk::PipelineStageFlagBits::eFragmentShader, {}, {},
                                                {}, toShaderReadBarriers);

    submitPendingUpload(std::move(pendingUpload));
}

vk::raii::CommandBuffer UploadContext::beginCommandBuffer() {
    if (!m_device || m_commandPool == nullptr) {
        throw std::runtime_error("UploadContext::beginCommandBuffer called before init.");
    }

    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = *m_commandPool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = 1;

    auto commandBuffers = m_device->allocateCommandBuffers(allocInfo);
    vk::raii::CommandBuffer commandBuffer = std::move(commandBuffers.front());

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    commandBuffer.begin(beginInfo);

    return commandBuffer;
}

void UploadContext::submitPendingUpload(PendingUpload &&pendingUpload) {
    if (!m_queue) {
        throw std::runtime_error("UploadContext::submitPendingUpload called before init.");
    }

    pendingUpload.commandBuffer.end();

    const vk::CommandBuffer rawCommandBuffer = *pendingUpload.commandBuffer;
    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &rawCommandBuffer;

    m_queue->submit(submitInfo, *pendingUpload.fence);
    m_pendingUploads.emplace_back(std::move(pendingUpload));
}
