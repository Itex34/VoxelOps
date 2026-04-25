#pragma once

#include <vulkan/vulkan_raii.hpp>

namespace VulkanUtils {

uint32_t findMemoryType(const vk::raii::PhysicalDevice &physicalDevice, uint32_t typeFilter,
                        vk::MemoryPropertyFlags properties);

void createBuffer(const vk::raii::Device &device, const vk::raii::PhysicalDevice &physicalDevice,
                  vk::DeviceSize size, vk::BufferUsageFlags usage,
                  vk::MemoryPropertyFlags properties, vk::raii::Buffer &buffer,
                  vk::raii::DeviceMemory &bufferMemory);

vk::raii::CommandBuffer beginSingleTimeCommands(const vk::raii::Device &device,
                                                const vk::raii::CommandPool &commandPool);

void endSingleTimeCommands(const vk::raii::Device &device, const vk::raii::Queue &queue,
                           vk::raii::CommandBuffer &&commandBuffer);

void copyBuffer(const vk::raii::Device &device, const vk::raii::CommandPool &commandPool,
                const vk::raii::Queue &queue, vk::Buffer srcBuffer, vk::Buffer dstBuffer,
                vk::DeviceSize size);

void transitionImageLayout(const vk::raii::Device &device, const vk::raii::CommandPool &commandPool,
                           const vk::raii::Queue &queue, vk::Image image, vk::ImageLayout oldLayout,
                           vk::ImageLayout newLayout);

void copyBufferToImage(const vk::raii::Device &device, const vk::raii::CommandPool &commandPool,
                       const vk::raii::Queue &queue, vk::Buffer buffer, vk::Image image,
                       uint32_t width, uint32_t height);

} // namespace VulkanUtils
