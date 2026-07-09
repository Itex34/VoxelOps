#pragma once

#include "graphics/Vulkan/vulkan/VulkanResource.hpp"

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

namespace VulkanUtils {

    void createBuffer(
        VmaAllocator allocator,
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags properties,
        VulkanBuffer &buffer,
        VmaAllocationCreateFlags allocationFlags = 0,
        const uint32_t *queueFamilyIndices = nullptr,
        uint32_t queueFamilyIndexCount = 0
    );

    void destroyBuffer(VulkanBuffer &buffer);

    void createImage(
        VmaAllocator allocator,
        const vk::ImageCreateInfo &imageInfo,
        vk::MemoryPropertyFlags properties,
        VulkanImage &image,
        VmaAllocationCreateFlags allocationFlags = 0
    );

    void destroyImage(VulkanImage &image);

    void *mapAllocation(VulkanBuffer &buffer);
    void unmapAllocation(VulkanBuffer &buffer);

    vk::raii::CommandBuffer beginSingleTimeCommands(
        const vk::raii::Device &device, const vk::raii::CommandPool &commandPool
    );

    void endSingleTimeCommands(
        const vk::raii::Device &device,
        const vk::raii::Queue &queue,
        vk::raii::CommandBuffer &&commandBuffer
    );

    void copyBuffer(
        const vk::raii::Device &device,
        const vk::raii::CommandPool &commandPool,
        const vk::raii::Queue &queue,
        vk::Buffer srcBuffer,
        vk::Buffer dstBuffer,
        vk::DeviceSize size
    );

    void transitionImageLayout(
        const vk::raii::Device &device,
        const vk::raii::CommandPool &commandPool,
        const vk::raii::Queue &queue,
        vk::Image image,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout
    );

    void copyBufferToImage(
        const vk::raii::Device &device,
        const vk::raii::CommandPool &commandPool,
        const vk::raii::Queue &queue,
        vk::Buffer buffer,
        vk::Image image,
        uint32_t width,
        uint32_t height
    );

    vk::raii::ShaderModule
    loadShaderModule(const vk::raii::Device &device, const std::string &path);
} // namespace VulkanUtils
