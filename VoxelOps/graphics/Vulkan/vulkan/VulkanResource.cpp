#include "graphics/Vulkan/vulkan/VulkanResource.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
    std::string vkResultMessage(const char *operation, VkResult result) {
        return std::string(operation) + " failed with VkResult " + std::to_string(result) + ".";
    }

    VmaAllocationCreateFlags allocationFlagsFor(vk::MemoryPropertyFlags properties) {
        if (static_cast<bool>(properties & vk::MemoryPropertyFlagBits::eHostVisible)) {
            return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        }
        return 0;
    }

    VkImageCreateInfo toVkImageCreateInfo(const vk::ImageCreateInfo &imageInfo) {
        VkImageCreateInfo raw{};
        raw.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        raw.pNext = imageInfo.pNext;
        raw.flags = static_cast<VkImageCreateFlags>(imageInfo.flags);
        raw.imageType = static_cast<VkImageType>(imageInfo.imageType);
        raw.format = static_cast<VkFormat>(imageInfo.format);
        raw.extent = VkExtent3D{
            imageInfo.extent.width,
            imageInfo.extent.height,
            imageInfo.extent.depth
        };
        raw.mipLevels = imageInfo.mipLevels;
        raw.arrayLayers = imageInfo.arrayLayers;
        raw.samples = static_cast<VkSampleCountFlagBits>(imageInfo.samples);
        raw.tiling = static_cast<VkImageTiling>(imageInfo.tiling);
        raw.usage = static_cast<VkImageUsageFlags>(imageInfo.usage);
        raw.sharingMode = static_cast<VkSharingMode>(imageInfo.sharingMode);
        raw.queueFamilyIndexCount = imageInfo.queueFamilyIndexCount;
        raw.pQueueFamilyIndices = imageInfo.pQueueFamilyIndices;
        raw.initialLayout = static_cast<VkImageLayout>(imageInfo.initialLayout);
        return raw;
    }
}

VulkanBuffer::~VulkanBuffer() {
    destroy();
}

VulkanBuffer::VulkanBuffer(VulkanBuffer &&other) noexcept {
    moveFrom(std::move(other));
}

VulkanBuffer &VulkanBuffer::operator=(VulkanBuffer &&other) noexcept {
    if (this != &other) {
        destroy();
        moveFrom(std::move(other));
    }
    return *this;
}

void VulkanBuffer::create(
    VmaAllocator allocator,
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags properties,
    VmaAllocationCreateFlags allocationFlags,
    const uint32_t *queueFamilyIndices,
    uint32_t queueFamilyIndexCount
) {
    destroy();
    if (allocator == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanBuffer::create requires a valid VMA allocator.");
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = std::max<vk::DeviceSize>(size, 4u);
    bufferInfo.usage = static_cast<VkBufferUsageFlags>(usage);
    if (queueFamilyIndices != nullptr && queueFamilyIndexCount > 1) {
        bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bufferInfo.queueFamilyIndexCount = queueFamilyIndexCount;
        bufferInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(properties);
    allocInfo.flags = allocationFlagsFor(properties) | allocationFlags;

    const VkResult result =
        vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_buffer, &m_allocation, nullptr);
    if (result != VK_SUCCESS || m_buffer == VK_NULL_HANDLE || m_allocation == VK_NULL_HANDLE) {
        if (m_buffer != VK_NULL_HANDLE || m_allocation != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, m_buffer, m_allocation);
        }
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        throw std::runtime_error(vkResultMessage("VulkanBuffer::create", result));
    }

    m_allocator = allocator;
    m_size = bufferInfo.size;
}

void VulkanBuffer::destroy() noexcept {
    if (m_allocator != VK_NULL_HANDLE && m_buffer != VK_NULL_HANDLE &&
        m_allocation != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_allocator = VK_NULL_HANDLE;
    m_buffer = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_size = 0;
}

void *VulkanBuffer::map() {
    if (m_allocator == VK_NULL_HANDLE || m_allocation == VK_NULL_HANDLE) {
        return nullptr;
    }
    void *mapped = nullptr;
    const VkResult result = vmaMapMemory(m_allocator, m_allocation, &mapped);
    if (result != VK_SUCCESS) {
        throw std::runtime_error(vkResultMessage("VulkanBuffer::map", result));
    }
    return mapped;
}

void VulkanBuffer::unmap() noexcept {
    if (m_allocator != VK_NULL_HANDLE && m_allocation != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_allocator, m_allocation);
    }
}

void VulkanBuffer::moveFrom(VulkanBuffer &&other) noexcept {
    m_allocator = other.m_allocator;
    m_buffer = other.m_buffer;
    m_allocation = other.m_allocation;
    m_size = other.m_size;

    other.m_allocator = VK_NULL_HANDLE;
    other.m_buffer = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_size = 0;
}

VulkanImage::~VulkanImage() {
    destroy();
}

VulkanImage::VulkanImage(VulkanImage &&other) noexcept {
    moveFrom(std::move(other));
}

VulkanImage &VulkanImage::operator=(VulkanImage &&other) noexcept {
    if (this != &other) {
        destroy();
        moveFrom(std::move(other));
    }
    return *this;
}

void VulkanImage::create(
    VmaAllocator allocator,
    const vk::ImageCreateInfo &imageInfo,
    vk::MemoryPropertyFlags properties,
    VmaAllocationCreateFlags allocationFlags
) {
    destroy();
    if (allocator == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanImage::create requires a valid VMA allocator.");
    }

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(properties);
    allocInfo.flags = allocationFlagsFor(properties) | allocationFlags;

    const VkImageCreateInfo rawImageInfo = toVkImageCreateInfo(imageInfo);
    const VkResult result =
        vmaCreateImage(allocator, &rawImageInfo, &allocInfo, &m_image, &m_allocation, nullptr);
    if (result != VK_SUCCESS || m_image == VK_NULL_HANDLE || m_allocation == VK_NULL_HANDLE) {
        if (m_image != VK_NULL_HANDLE || m_allocation != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, m_image, m_allocation);
        }
        m_image = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        throw std::runtime_error(vkResultMessage("VulkanImage::create", result));
    }

    m_allocator = allocator;
    m_format = imageInfo.format;
    m_extent = imageInfo.extent;
}

void VulkanImage::destroy() noexcept {
    if (m_allocator != VK_NULL_HANDLE && m_image != VK_NULL_HANDLE &&
        m_allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_allocator = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_format = vk::Format::eUndefined;
    m_extent = vk::Extent3D{};
}

void VulkanImage::moveFrom(VulkanImage &&other) noexcept {
    m_allocator = other.m_allocator;
    m_image = other.m_image;
    m_allocation = other.m_allocation;
    m_format = other.m_format;
    m_extent = other.m_extent;

    other.m_allocator = VK_NULL_HANDLE;
    other.m_image = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_format = vk::Format::eUndefined;
    other.m_extent = vk::Extent3D{};
}
