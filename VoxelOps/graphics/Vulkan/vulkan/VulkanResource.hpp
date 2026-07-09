#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <cstddef>

class VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer &) = delete;
    VulkanBuffer &operator=(const VulkanBuffer &) = delete;
    VulkanBuffer(VulkanBuffer &&other) noexcept;
    VulkanBuffer &operator=(VulkanBuffer &&other) noexcept;

    void create(
        VmaAllocator allocator,
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags properties,
        VmaAllocationCreateFlags allocationFlags = 0,
        const uint32_t *queueFamilyIndices = nullptr,
        uint32_t queueFamilyIndexCount = 0
    );
    void destroy() noexcept;

    void *map();
    void unmap() noexcept;

    VkBuffer raw() const noexcept {
        return m_buffer;
    }
    vk::Buffer handle() const noexcept {
        return vk::Buffer(m_buffer);
    }
    operator VkBuffer() const noexcept {
        return m_buffer;
    }
    operator vk::Buffer() const noexcept {
        return handle();
    }
    vk::Buffer operator*() const noexcept {
        return handle();
    }
    explicit operator bool() const noexcept {
        return m_buffer != VK_NULL_HANDLE;
    }
    bool operator==(std::nullptr_t) const noexcept {
        return m_buffer == VK_NULL_HANDLE;
    }
    bool operator!=(std::nullptr_t) const noexcept {
        return m_buffer != VK_NULL_HANDLE;
    }
    vk::DeviceSize size() const noexcept {
        return m_size;
    }
    VmaAllocation allocation() const noexcept {
        return m_allocation;
    }

private:
    void moveFrom(VulkanBuffer &&other) noexcept;

    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    vk::DeviceSize m_size = 0;
};

class VulkanImage {
public:
    VulkanImage() = default;
    ~VulkanImage();

    VulkanImage(const VulkanImage &) = delete;
    VulkanImage &operator=(const VulkanImage &) = delete;
    VulkanImage(VulkanImage &&other) noexcept;
    VulkanImage &operator=(VulkanImage &&other) noexcept;

    void create(
        VmaAllocator allocator,
        const vk::ImageCreateInfo &imageInfo,
        vk::MemoryPropertyFlags properties,
        VmaAllocationCreateFlags allocationFlags = 0
    );
    void destroy() noexcept;

    VkImage raw() const noexcept {
        return m_image;
    }
    vk::Image handle() const noexcept {
        return vk::Image(m_image);
    }
    vk::Image operator*() const noexcept {
        return handle();
    }
    explicit operator bool() const noexcept {
        return m_image != VK_NULL_HANDLE;
    }
    bool operator==(std::nullptr_t) const noexcept {
        return m_image == VK_NULL_HANDLE;
    }
    bool operator!=(std::nullptr_t) const noexcept {
        return m_image != VK_NULL_HANDLE;
    }
    vk::Format format() const noexcept {
        return m_format;
    }
    vk::Extent3D extent() const noexcept {
        return m_extent;
    }
    VmaAllocation allocation() const noexcept {
        return m_allocation;
    }

private:
    void moveFrom(VulkanImage &&other) noexcept;

    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    vk::Format m_format = vk::Format::eUndefined;
    vk::Extent3D m_extent{};
};
