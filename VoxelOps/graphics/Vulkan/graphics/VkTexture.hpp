#pragma once

#include <vulkan/vulkan_raii.hpp>

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
        return m_descriptorSets.empty() ? VK_NULL_HANDLE : *m_descriptorSets[0];
    }

private:
    vk::raii::Image m_image{nullptr};
    vk::raii::DeviceMemory m_imageMemory{nullptr};
    vk::raii::ImageView m_imageView{nullptr};
    vk::raii::Sampler m_sampler{nullptr};

    vk::raii::DescriptorSetLayout m_descriptorSetLayout{nullptr};
    vk::raii::DescriptorPool m_descriptorPool{nullptr};
    std::vector<vk::raii::DescriptorSet> m_descriptorSets;
};
