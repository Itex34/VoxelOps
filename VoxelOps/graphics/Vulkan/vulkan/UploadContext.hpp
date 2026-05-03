#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <vector>

class UploadContext {
public:
    struct StagingBuffer {
        vk::raii::Buffer buffer{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
    };

    void
    init(const vk::raii::Device &device, uint32_t queueFamilyIndex, const vk::raii::Queue &queue);
    void cleanup();

    void poll();
    void waitIdle();

    StagingBuffer createStagingBuffer(
        const vk::raii::PhysicalDevice &physicalDevice, const void *data, vk::DeviceSize size
    );

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
    vk::raii::CommandPool m_commandPool{nullptr};
    std::vector<PendingUpload> m_pendingUploads;

    vk::raii::CommandBuffer beginCommandBuffer();
    void submitPendingUpload(PendingUpload &&pendingUpload);
};
