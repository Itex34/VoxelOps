#pragma once

#include "graphics/Vulkan/vulkan/VulkanResource.hpp"

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

class UploadContext {
public:
    struct StagingBuffer {
        VulkanBuffer buffer;
        vk::DeviceSize capacity = 0;
    };

    void
    init(
        const vk::raii::Device &device,
        VmaAllocator allocator,
        uint32_t queueFamilyIndex,
        const vk::raii::Queue &queue
    );
    void cleanup();

    void poll();
    void waitIdle();

    VmaAllocator allocator() const noexcept {
        return m_allocator;
    }

    StagingBuffer createStagingBuffer(const void *data, vk::DeviceSize size);

    struct BufferCopyUpload {
        StagingBuffer stagingBuffer{};
        vk::Buffer dstBuffer = VK_NULL_HANDLE;
        vk::DeviceSize size = 0;
    };

    void submitCopyBuffer(StagingBuffer &&stagingBuffer, vk::Buffer dstBuffer, vk::DeviceSize size);
    void submitCopyBufferBatch(std::vector<BufferCopyUpload> &&uploads);
    void submitImageUpload(
        StagingBuffer &&stagingBuffer, vk::Image image, uint32_t width, uint32_t height
    );
    void submitImageUploadArray(
        StagingBuffer &&stagingBuffer,
        vk::Image image,
        uint32_t width,
        uint32_t height,
        uint32_t layerCount
    );

private:
    struct PendingUpload {
        vk::raii::Fence fence{nullptr};
        vk::raii::CommandBuffer commandBuffer{nullptr};
        std::vector<StagingBuffer> stagingBuffers;
    };

    const vk::raii::Device *m_device = nullptr;
    const vk::raii::Queue *m_queue = nullptr;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    vk::raii::CommandPool m_commandPool{nullptr};
    std::vector<PendingUpload> m_pendingUploads;
    std::vector<StagingBuffer> m_reusableStagingBuffers;
    vk::DeviceSize m_reusableStagingBytes = 0;

    vk::raii::CommandBuffer beginCommandBuffer();
    void submitPendingUpload(PendingUpload &&pendingUpload);
    StagingBuffer acquireReusableStagingBuffer(vk::DeviceSize minimumSize);
    void recycleStagingBuffer(StagingBuffer &&stagingBuffer);
};
