#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace VulkanUtils {

uint32_t findMemoryType(const vk::raii::PhysicalDevice &physicalDevice, uint32_t typeFilter,
                        vk::MemoryPropertyFlags properties) {
    const vk::PhysicalDeviceMemoryProperties memoryProperties =
        physicalDevice.getMemoryProperties();
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeMatches = (typeFilter & (1u << i)) != 0;
        const bool propertyMatches =
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && propertyMatches) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type.");
}

void createBuffer(const vk::raii::Device &device, const vk::raii::PhysicalDevice &physicalDevice,
                  vk::DeviceSize size, vk::BufferUsageFlags usage,
                  vk::MemoryPropertyFlags properties, vk::raii::Buffer &buffer,
                  vk::raii::DeviceMemory &bufferMemory) {
    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;

    buffer = vk::raii::Buffer(device, bufferInfo);
    const vk::MemoryRequirements memoryRequirements = buffer.getMemoryRequirements();

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(physicalDevice, memoryRequirements.memoryTypeBits, properties);
    bufferMemory = vk::raii::DeviceMemory(device, allocInfo);
    buffer.bindMemory(*bufferMemory, 0);
}

vk::raii::CommandBuffer beginSingleTimeCommands(const vk::raii::Device &device,
                                                const vk::raii::CommandPool &commandPool) {
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandPool = *commandPool;
    allocInfo.commandBufferCount = 1;

    auto commandBuffers = device.allocateCommandBuffers(allocInfo);
    vk::raii::CommandBuffer commandBuffer = std::move(commandBuffers.front());

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    commandBuffer.begin(beginInfo);

    return commandBuffer;
}

void endSingleTimeCommands(const vk::raii::Device &device, const vk::raii::Queue &queue,
                           vk::raii::CommandBuffer &&commandBuffer) {
    commandBuffer.end();

    vk::CommandBuffer raw = *commandBuffer;
    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &raw;

    vk::raii::Fence fence(device, vk::FenceCreateInfo{});
    queue.submit(submitInfo, *fence);

    (void)device.waitForFences(*fence, vk::True, std::numeric_limits<uint64_t>::max());
}

void copyBuffer(const vk::raii::Device &device, const vk::raii::CommandPool &commandPool,
                const vk::raii::Queue &queue, vk::Buffer srcBuffer, vk::Buffer dstBuffer,
                vk::DeviceSize size) {
    vk::raii::CommandBuffer commandBuffer = beginSingleTimeCommands(device, commandPool);

    vk::BufferCopy region{};
    region.size = size;
    std::array<vk::BufferCopy, 1> regions = {region};
    commandBuffer.copyBuffer(srcBuffer, dstBuffer, regions);

    endSingleTimeCommands(device, queue, std::move(commandBuffer));
}

void transitionImageLayout(const vk::raii::Device &device, const vk::raii::CommandPool &commandPool,
                           const vk::raii::Queue &queue, vk::Image image, vk::ImageLayout oldLayout,
                           vk::ImageLayout newLayout) {
    vk::raii::CommandBuffer commandBuffer = beginSingleTimeCommands(device, commandPool);

    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vk::PipelineStageFlags srcStage;
    vk::PipelineStageFlags dstStage;

    if (oldLayout == vk::ImageLayout::eUndefined &&
        newLayout == vk::ImageLayout::eTransferDstOptimal) {
        barrier.srcAccessMask = {};
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
        dstStage = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
               newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        srcStage = vk::PipelineStageFlagBits::eTransfer;
        dstStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else {
        throw std::runtime_error("Unsupported image layout transition.");
    }

    std::array<vk::ImageMemoryBarrier, 1> barriers = {barrier};
    commandBuffer.pipelineBarrier(srcStage, dstStage, {}, {}, {}, barriers);

    endSingleTimeCommands(device, queue, std::move(commandBuffer));
}

void copyBufferToImage(const vk::raii::Device &device, const vk::raii::CommandPool &commandPool,
                       const vk::raii::Queue &queue, vk::Buffer buffer, vk::Image image,
                       uint32_t width, uint32_t height) {
    vk::raii::CommandBuffer commandBuffer = beginSingleTimeCommands(device, commandPool);

    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D{0, 0, 0};
    region.imageExtent = vk::Extent3D{width, height, 1};

    std::array<vk::BufferImageCopy, 1> regions = {region};
    commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, regions);

    endSingleTimeCommands(device, queue, std::move(commandBuffer));
}

} // namespace VulkanUtils
