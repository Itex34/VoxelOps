#include "graphics/Vulkan/graphics/VkTexture.hpp"

#include "graphics/Vulkan/vulkan/UploadContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr uint32_t kFallbackTextureWidth = 64;
    constexpr uint32_t kFallbackTextureHeight = 64;
    constexpr uint32_t kBytesPerPixelRgba8 = 4;

    struct TextureData {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
    };

    struct TextureArrayData {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 0;
        std::vector<uint8_t> pixels;
    };

    TextureData loadTextureFromFile(const std::string &texturePath) {
        int width = 0;
        int height = 0;
        int channels = 0;

        stbi_uc *loadedPixels =
            stbi_load(texturePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!loadedPixels || width <= 0 || height <= 0) {
            if (loadedPixels) {
                stbi_image_free(loadedPixels);
            }

            TextureData fallback{};
            fallback.width = kFallbackTextureWidth;
            fallback.height = kFallbackTextureHeight;
            fallback.pixels.resize(
                static_cast<size_t>(fallback.width) * fallback.height * kBytesPerPixelRgba8
            );

            for (uint32_t y = 0; y < fallback.height; ++y) {
                for (uint32_t x = 0; x < fallback.width; ++x) {
                    const bool checker = ((x / 8u) + (y / 8u)) % 2u == 0u;
                    const uint8_t value = checker ? 220u : 40u;
                    const size_t index =
                        (static_cast<size_t>(y) * fallback.width + x) * kBytesPerPixelRgba8;
                    fallback.pixels[index + 0] = value;
                    fallback.pixels[index + 1] = checker ? 30u : 20u;
                    fallback.pixels[index + 2] = checker ? 220u : 20u;
                    fallback.pixels[index + 3] = 255u;
                }
            }

            if (!texturePath.empty()) {
                const char *stbReason = stbi_failure_reason();
                std::cerr << "Warning: failed to load texture '" << texturePath << "'";
                if (stbReason) {
                    std::cerr << " (" << stbReason << ")";
                }
                std::cerr << ". Using fallback texture.\n";
            }

            return fallback;
        }

        (void)channels;

        TextureData textureData{};
        textureData.width = static_cast<uint32_t>(width);
        textureData.height = static_cast<uint32_t>(height);
        textureData.pixels.resize(
            static_cast<size_t>(textureData.width) * textureData.height * kBytesPerPixelRgba8
        );
        std::memcpy(textureData.pixels.data(), loadedPixels, textureData.pixels.size());
        stbi_image_free(loadedPixels);

        return textureData;
    }

    TextureArrayData buildFallbackAtlasArray(uint32_t tilesPerAxis) {
        TextureArrayData fallback{};
        const uint32_t tileResolution = 16;
        fallback.width = tileResolution;
        fallback.height = tileResolution;
        fallback.layers = tilesPerAxis * tilesPerAxis;
        fallback.pixels.resize(
            static_cast<size_t>(fallback.width) * fallback.height * kBytesPerPixelRgba8 *
            static_cast<size_t>(fallback.layers)
        );

        for (uint32_t layer = 0; layer < fallback.layers; ++layer) {
            const uint8_t tint = static_cast<uint8_t>(30u + (layer * 17u) % 200u);
            for (uint32_t y = 0; y < fallback.height; ++y) {
                for (uint32_t x = 0; x < fallback.width; ++x) {
                    const bool checker = ((x / 4u) + (y / 4u)) % 2u == 0u;
                    const size_t pixelIndex =
                        ((static_cast<size_t>(layer) * fallback.height + y) * fallback.width + x) *
                        kBytesPerPixelRgba8;
                    fallback.pixels[pixelIndex + 0] =
                        checker ? tint : static_cast<uint8_t>(255u - tint);
                    fallback.pixels[pixelIndex + 1] = checker ? 20u : 180u;
                    fallback.pixels[pixelIndex + 2] = checker ? 220u : 20u;
                    fallback.pixels[pixelIndex + 3] = 255u;
                }
            }
        }

        return fallback;
    }

    TextureArrayData loadAtlasArrayFromFile(const std::string &texturePath, uint32_t tilesPerAxis) {
        if (tilesPerAxis == 0) {
            throw std::runtime_error("tilesPerAxis must be > 0 for atlas array upload.");
        }

        int width = 0;
        int height = 0;
        int channels = 0;

        // Keep parity with OpenGL atlas loading path.
        stbi_set_flip_vertically_on_load(true);
        stbi_uc *loadedPixels =
            stbi_load(texturePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        stbi_set_flip_vertically_on_load(false);

        if (!loadedPixels || width <= 0 || height <= 0) {
            if (loadedPixels) {
                stbi_image_free(loadedPixels);
            }

            if (!texturePath.empty()) {
                const char *stbReason = stbi_failure_reason();
                std::cerr << "Warning: failed to load atlas texture '" << texturePath << "'";
                if (stbReason) {
                    std::cerr << " (" << stbReason << ")";
                }
                std::cerr << ". Using fallback atlas array.\n";
            }
            return buildFallbackAtlasArray(tilesPerAxis);
        }

        (void)channels;

        if (width != height || (width % static_cast<int>(tilesPerAxis)) != 0) {
            std::cerr << "Warning: atlas texture dimensions " << width << "x" << height
                      << " are incompatible with tilesPerAxis=" << tilesPerAxis
                      << ". Using fallback atlas array.\n";
            stbi_image_free(loadedPixels);
            return buildFallbackAtlasArray(tilesPerAxis);
        }

        const uint32_t tileResolution =
            static_cast<uint32_t>(width / static_cast<int>(tilesPerAxis));
        const uint32_t layers = tilesPerAxis * tilesPerAxis;
        const size_t tilePixelBytes =
            static_cast<size_t>(tileResolution) * tileResolution * kBytesPerPixelRgba8;

        TextureArrayData out{};
        out.width = tileResolution;
        out.height = tileResolution;
        out.layers = layers;
        out.pixels.resize(tilePixelBytes * static_cast<size_t>(layers));

        const size_t sourceRowBytes = static_cast<size_t>(width) * kBytesPerPixelRgba8;
        const size_t tileRowBytes = static_cast<size_t>(tileResolution) * kBytesPerPixelRgba8;

        for (uint32_t tileY = 0; tileY < tilesPerAxis; ++tileY) {
            for (uint32_t tileX = 0; tileX < tilesPerAxis; ++tileX) {
                const uint32_t layer = tileY * tilesPerAxis + tileX;
                for (uint32_t row = 0; row < tileResolution; ++row) {
                    const size_t srcY = static_cast<size_t>(tileY) * tileResolution + row;
                    const size_t srcOffset =
                        srcY * sourceRowBytes + static_cast<size_t>(tileX) * tileRowBytes;
                    const size_t dstOffset =
                        static_cast<size_t>(layer) * tilePixelBytes + row * tileRowBytes;
                    std::memcpy(
                        out.pixels.data() + dstOffset, loadedPixels + srcOffset, tileRowBytes
                    );
                }
            }
        }

        stbi_image_free(loadedPixels);
        return out;
    }

    void createDescriptorResources(
        const vk::raii::Device &device,
        vk::raii::ImageView &imageView,
        vk::raii::Sampler &sampler,
        vk::raii::DescriptorSetLayout &descriptorSetLayout,
        vk::raii::DescriptorPool &descriptorPool,
        vk::DescriptorSet &descriptorSet
    ) {
        vk::DescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;
        samplerBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;

        vk::DescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &samplerBinding;
        descriptorSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);

        vk::DescriptorPoolSize poolSize{};
        poolSize.type = vk::DescriptorType::eCombinedImageSampler;
        poolSize.descriptorCount = 1;

        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        descriptorPool = vk::raii::DescriptorPool(device, poolInfo);

        const vk::DescriptorSetLayout setLayout = *descriptorSetLayout;
        vk::DescriptorSetAllocateInfo setAllocInfo{};
        setAllocInfo.descriptorPool = *descriptorPool;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &setLayout;
        std::vector<vk::raii::DescriptorSet> descriptorSets =
            device.allocateDescriptorSets(setAllocInfo);
        if (descriptorSets.empty()) {
            throw std::runtime_error("Failed to allocate texture descriptor set.");
        }
        descriptorSet = descriptorSets[0].release();

        vk::DescriptorImageInfo descriptorImageInfo{};
        descriptorImageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        descriptorImageInfo.imageView = *imageView;
        descriptorImageInfo.sampler = *sampler;

        vk::WriteDescriptorSet write{};
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.descriptorCount = 1;
        write.pImageInfo = &descriptorImageInfo;

        const std::array<vk::WriteDescriptorSet, 1> writes = {write};
        device.updateDescriptorSets(writes, {});
    }
} // namespace

void VkTexture::initFromFile(
    const vk::raii::Device &device,
    const vk::raii::PhysicalDevice &physicalDevice,
    UploadContext &uploadContext,
    const std::string &texturePath,
    bool samplerAnisotropyEnabled,
    float maxSamplerAnisotropy
) {
    cleanup();
    (void)physicalDevice;
    const TextureData textureData = loadTextureFromFile(texturePath);

    const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(textureData.pixels.size());

    UploadContext::StagingBuffer stagingBuffer =
        uploadContext.createStagingBuffer(textureData.pixels.data(), imageSize);

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent = vk::Extent3D(textureData.width, textureData.height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = vk::Format::eR8G8B8A8Srgb;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    VulkanUtils::createImage(
        uploadContext.allocator(),
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        m_image
    );

    uploadContext.submitImageUpload(
        std::move(stagingBuffer), *m_image, textureData.width, textureData.height
    );

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = *m_image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = vk::Format::eR8G8B8A8Srgb;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    m_imageView = vk::raii::ImageView(device, viewInfo);

    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
    samplerInfo.anisotropyEnable = samplerAnisotropyEnabled ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy =
        samplerAnisotropyEnabled ? std::min(16.0f, maxSamplerAnisotropy) : 1.0f;
    samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    m_sampler = vk::raii::Sampler(device, samplerInfo);

    createDescriptorResources(
        device, m_imageView, m_sampler, m_descriptorSetLayout, m_descriptorPool, m_descriptorSet
    );
}

void VkTexture::initFromAtlasFileAsArray(
    const vk::raii::Device &device,
    const vk::raii::PhysicalDevice &physicalDevice,
    UploadContext &uploadContext,
    const std::string &texturePath,
    uint32_t tilesPerAxis,
    bool samplerAnisotropyEnabled,
    float maxSamplerAnisotropy
) {
    cleanup();
    (void)physicalDevice;
    const TextureArrayData textureArray = loadAtlasArrayFromFile(texturePath, tilesPerAxis);
    const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(textureArray.pixels.size());

    UploadContext::StagingBuffer stagingBuffer =
        uploadContext.createStagingBuffer(textureArray.pixels.data(), imageSize);

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent = vk::Extent3D(textureArray.width, textureArray.height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = textureArray.layers;
    imageInfo.format = vk::Format::eR8G8B8A8Srgb;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    VulkanUtils::createImage(
        uploadContext.allocator(),
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        m_image
    );

    uploadContext.submitImageUploadArray(
        std::move(stagingBuffer),
        *m_image,
        textureArray.width,
        textureArray.height,
        textureArray.layers
    );

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = *m_image;
    viewInfo.viewType = vk::ImageViewType::e2DArray;
    viewInfo.format = vk::Format::eR8G8B8A8Srgb;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = textureArray.layers;
    m_imageView = vk::raii::ImageView(device, viewInfo);

    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
    samplerInfo.anisotropyEnable = samplerAnisotropyEnabled ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy =
        samplerAnisotropyEnabled ? std::min(16.0f, maxSamplerAnisotropy) : 1.0f;
    samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    m_sampler = vk::raii::Sampler(device, samplerInfo);

    createDescriptorResources(
        device, m_imageView, m_sampler, m_descriptorSetLayout, m_descriptorPool, m_descriptorSet
    );
}

void VkTexture::cleanup() {
    m_descriptorSet = VK_NULL_HANDLE;
    m_descriptorPool.clear();
    m_descriptorSetLayout.clear();

    m_sampler.clear();
    m_imageView.clear();
    VulkanUtils::destroyImage(m_image);
}
