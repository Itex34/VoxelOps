#pragma once

#include "../../../ui/native/NativeUiDrawData.hpp"
#include "graphics/Vulkan/vulkan/VulkanResource.hpp"

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

class UploadContext;
class VulkanContext;

class VulkanNativeUiRenderer {
public:
    VulkanNativeUiRenderer() = default;
    ~VulkanNativeUiRenderer() = default;

    VulkanNativeUiRenderer(const VulkanNativeUiRenderer &) = delete;
    VulkanNativeUiRenderer &operator=(const VulkanNativeUiRenderer &) = delete;

    void initialize(
        VulkanContext &context,
        UploadContext &uploadContext,
        vk::RenderPass renderPass,
        uint32_t swapchainImageCount
    );
    void cleanup();

    void render(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const NativeUiDrawData &drawData,
        vk::Extent2D extent
    );

    bool isInitialized() const noexcept {
        return m_initialized;
    }

private:
    struct PushConstants {
        float screenSize[2]{1.0f, 1.0f};
        uint32_t textureMode = 0;
        uint32_t _pad = 0;
    };

    struct TextureResource {
        NativeUiTextureFormat format = NativeUiTextureFormat::R8;
        int width = 0;
        int height = 0;
        VulkanImage image;
        vk::raii::ImageView view{nullptr};
        vk::raii::Sampler sampler{nullptr};
        vk::raii::DescriptorSet descriptorSet{nullptr};
    };

    struct PerImageResources {
        VulkanBuffer vertexBuffer;
        vk::DeviceSize vertexCapacityBytes = 0;
        void *vertexMapped = nullptr;

        VulkanBuffer indexBuffer;
        vk::DeviceSize indexCapacityBytes = 0;
        void *indexMapped = nullptr;
    };

    void createPipeline(vk::RenderPass renderPass);
    void createDescriptorResources(uint32_t maxTextureCount);
    void ensureTexture(const NativeUiTextureUpload &upload);
    void createTexture(NativeUiTextureHandle handle, const NativeUiTextureUpload &upload);
    void createFallbackTexture();
    void ensureBufferCapacity(uint32_t imageIndex, vk::DeviceSize vertexBytes, vk::DeviceSize indexBytes);
    void cleanupPerImageResources();

    VulkanContext *m_context = nullptr;
    UploadContext *m_uploadContext = nullptr;
    bool m_initialized = false;

    vk::raii::DescriptorSetLayout m_descriptorSetLayout{nullptr};
    vk::raii::DescriptorPool m_descriptorPool{nullptr};
    vk::raii::PipelineLayout m_pipelineLayout{nullptr};
    vk::raii::Pipeline m_pipeline{nullptr};
    std::unordered_map<NativeUiTextureHandle, TextureResource> m_textures;
    std::vector<PerImageResources> m_perImage;
};
