#pragma once

#include "graphics/Vulkan/vulkan/VulkanResource.hpp"

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <string>
#include <vector>

class UploadContext;

class VkTexture {
public:
    void initFromFile(
        const vk::raii::Device &device,
        const vk::raii::PhysicalDevice &physicalDevice,
        UploadContext &uploadContext,
        const std::string &texturePath,
        bool samplerAnisotropyEnabled,
        float maxSamplerAnisotropy
    );
    void initFromAtlasFileAsArray(
        const vk::raii::Device &device,
        const vk::raii::PhysicalDevice &physicalDevice,
        UploadContext &uploadContext,
        const std::string &texturePath,
        uint32_t tilesPerAxis,
        bool samplerAnisotropyEnabled,
        float maxSamplerAnisotropy
    );

    void cleanup();

    const vk::raii::DescriptorSetLayout &getDescriptorSetLayout() const {
        return m_descriptorSetLayout;
    }
    vk::DescriptorSet getDescriptorSet() const {
        return m_descriptorSet;
    }
    vk::ImageView getImageView() const {
        return (m_imageView != nullptr) ? *m_imageView : VK_NULL_HANDLE;
    }
    vk::Sampler getSampler() const {
        return (m_sampler != nullptr) ? *m_sampler : VK_NULL_HANDLE;
    }

private:
    VulkanImage m_image;
    vk::raii::ImageView m_imageView{nullptr};
    vk::raii::Sampler m_sampler{nullptr};

    vk::raii::DescriptorSetLayout m_descriptorSetLayout{nullptr};
    vk::raii::DescriptorPool m_descriptorPool{nullptr};
    vk::DescriptorSet m_descriptorSet = VK_NULL_HANDLE;
};
