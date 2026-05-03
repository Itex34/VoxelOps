#include "VulkanRenderer.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "../vulkan/VulkanUtils.hpp"
#include "NrdBootstrap.hpp"
#include "../graphics/Model.hpp"
#include "RestirConfig.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

using namespace Vulkan::Restir;

namespace {
    constexpr bool kRestirGiDispatchSpatialPass = false;

    struct alignas(16) RestirGiSpatialPushConstants {
        glm::uvec2 extent{0u, 0u};
        glm::vec2 invExtent{0.0f, 0.0f};
        float spatialWeight = 0.10f;
        float lumaPhi = 2.0f;
        float normalPower = 12.0f;
        float depthScale = 1200.0f;
    };
} // namespace

void VulkanRenderer::createRestirGiSpatialResources() {
    cleanupRestirGiSpatialResources();

    if (m_framebuffers.empty() || m_commandPool == nullptr) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();
    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0) {
        return;
    }

    const size_t imageCount = m_framebuffers.size();
    m_restirGiSpatialPerImage.resize(imageCount);
    m_restirGiSpatialMetaPerImage.resize(imageCount);

    constexpr vk::Format kGiSpatialFormat = vk::Format::eR16G16B16A16Sfloat;
    for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
        for (uint32_t parity = 0; parity < 2u; ++parity) {
            RestirDiReservoirResources &reservoir = m_restirGiSpatialPerImage[imageIndex][parity];
            RestirMetaResources &meta = m_restirGiSpatialMetaPerImage[imageIndex][parity];

            vk::ImageCreateInfo imageInfo{};
            imageInfo.imageType = vk::ImageType::e2D;
            imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1u);
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = kGiSpatialFormat;
            imageInfo.tiling = vk::ImageTiling::eOptimal;
            imageInfo.initialLayout = vk::ImageLayout::eUndefined;
            imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                              vk::ImageUsageFlagBits::eTransferDst;
            imageInfo.samples = vk::SampleCountFlagBits::e1;
            imageInfo.sharingMode = vk::SharingMode::eExclusive;

            reservoir.image = vk::raii::Image(device, imageInfo);
            {
                const vk::MemoryRequirements requirements = reservoir.image.getMemoryRequirements();
                vk::MemoryAllocateInfo allocInfo{};
                allocInfo.allocationSize = requirements.size;
                allocInfo.memoryTypeIndex = VulkanUtils::findMemoryType(
                    physicalDevice,
                    requirements.memoryTypeBits,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                reservoir.memory = vk::raii::DeviceMemory(device, allocInfo);
                reservoir.image.bindMemory(*reservoir.memory, 0);
            }

            vk::ImageViewCreateInfo reservoirViewInfo{};
            reservoirViewInfo.image = *reservoir.image;
            reservoirViewInfo.viewType = vk::ImageViewType::e2D;
            reservoirViewInfo.format = kGiSpatialFormat;
            reservoirViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            reservoirViewInfo.subresourceRange.baseMipLevel = 0;
            reservoirViewInfo.subresourceRange.levelCount = 1;
            reservoirViewInfo.subresourceRange.baseArrayLayer = 0;
            reservoirViewInfo.subresourceRange.layerCount = 1;
            reservoir.view = vk::raii::ImageView(device, reservoirViewInfo);

            meta.image = vk::raii::Image(device, imageInfo);
            {
                const vk::MemoryRequirements requirements = meta.image.getMemoryRequirements();
                vk::MemoryAllocateInfo allocInfo{};
                allocInfo.allocationSize = requirements.size;
                allocInfo.memoryTypeIndex = VulkanUtils::findMemoryType(
                    physicalDevice,
                    requirements.memoryTypeBits,
                    vk::MemoryPropertyFlagBits::eDeviceLocal
                );
                meta.memory = vk::raii::DeviceMemory(device, allocInfo);
                meta.image.bindMemory(*meta.memory, 0);
            }

            vk::ImageViewCreateInfo metaViewInfo{};
            metaViewInfo.image = *meta.image;
            metaViewInfo.viewType = vk::ImageViewType::e2D;
            metaViewInfo.format = kGiSpatialFormat;
            metaViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            metaViewInfo.subresourceRange.baseMipLevel = 0;
            metaViewInfo.subresourceRange.levelCount = 1;
            metaViewInfo.subresourceRange.baseArrayLayer = 0;
            metaViewInfo.subresourceRange.layerCount = 1;
            meta.view = vk::raii::ImageView(device, metaViewInfo);
        }
    }

    vk::DescriptorSetLayoutBinding temporalGiBinding{};
    temporalGiBinding.binding = 0;
    temporalGiBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    temporalGiBinding.descriptorCount = 1;
    temporalGiBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

    vk::DescriptorSetLayoutBinding temporalGiMetaBinding{};
    temporalGiMetaBinding.binding = 1;
    temporalGiMetaBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    temporalGiMetaBinding.descriptorCount = 1;
    temporalGiMetaBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

    vk::DescriptorSetLayoutBinding temporalValidationBinding{};
    temporalValidationBinding.binding = 2;
    temporalValidationBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    temporalValidationBinding.descriptorCount = 1;
    temporalValidationBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

    vk::DescriptorSetLayoutBinding spatialGiBinding{};
    spatialGiBinding.binding = 3;
    spatialGiBinding.descriptorType = vk::DescriptorType::eStorageImage;
    spatialGiBinding.descriptorCount = 1;
    spatialGiBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

    vk::DescriptorSetLayoutBinding spatialGiMetaBinding{};
    spatialGiMetaBinding.binding = 4;
    spatialGiMetaBinding.descriptorType = vk::DescriptorType::eStorageImage;
    spatialGiMetaBinding.descriptorCount = 1;
    spatialGiMetaBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

    const std::array<vk::DescriptorSetLayoutBinding, 5> bindings = {
        temporalGiBinding,
        temporalGiMetaBinding,
        temporalValidationBinding,
        spatialGiBinding,
        spatialGiMetaBinding
    };

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    m_restirGiSpatialDescriptorSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);

    std::array<vk::DescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = vk::DescriptorType::eCombinedImageSampler;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(imageCount) * 3u;
    poolSizes[1].type = vk::DescriptorType::eStorageImage;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(imageCount) * 2u;

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = static_cast<uint32_t>(imageCount);
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    m_restirGiSpatialDescriptorPool = vk::raii::DescriptorPool(device, poolInfo);

    std::vector<vk::DescriptorSetLayout> layouts(imageCount, *m_restirGiSpatialDescriptorSetLayout);
    vk::DescriptorSetAllocateInfo allocInfo{};
    allocInfo.descriptorPool = *m_restirGiSpatialDescriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();
    m_restirGiSpatialDescriptorSets = device.allocateDescriptorSets(allocInfo);

    std::string shaderDir;
#ifdef SHADER_DIR
    shaderDir = SHADER_DIR;
#endif
    if (!shaderDir.empty() && shaderDir.back() != '/' && shaderDir.back() != '\\') {
        shaderDir.push_back('/');
    }
    vk::raii::ShaderModule computeShader =
        VulkanUtils::loadShaderModule(device, shaderDir + "restir_gi_spatial.comp.spv");

    vk::PushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushConstantRange.offset = 0;
    pushConstantRange.size = static_cast<uint32_t>(sizeof(RestirGiSpatialPushConstants));

    const std::array<vk::DescriptorSetLayout, 1> computeLayouts = {
        *m_restirGiSpatialDescriptorSetLayout
    };
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(computeLayouts.size());
    pipelineLayoutInfo.pSetLayouts = computeLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    m_restirGiSpatialPipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

    vk::PipelineShaderStageCreateInfo stageInfo{};
    stageInfo.stage = vk::ShaderStageFlagBits::eCompute;
    stageInfo.module = *computeShader;
    stageInfo.pName = "main";

    vk::ComputePipelineCreateInfo computeInfo{};
    computeInfo.stage = stageInfo;
    computeInfo.layout = *m_restirGiSpatialPipelineLayout;
    m_restirGiSpatialPipeline = vk::raii::Pipeline(device, nullptr, computeInfo);

    vk::raii::CommandBuffer commandBuffer =
        VulkanUtils::beginSingleTimeCommands(device, m_commandPool);
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    const vk::ClearColorValue clearZero(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearMeta(std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0e-4f});
    for (const auto &perImage : m_restirGiSpatialPerImage) {
        for (const RestirDiReservoirResources &reservoir : perImage) {
            vk::ImageMemoryBarrier toGeneral{};
            toGeneral.oldLayout = vk::ImageLayout::eUndefined;
            toGeneral.newLayout = vk::ImageLayout::eGeneral;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = *reservoir.image;
            toGeneral.subresourceRange = range;
            toGeneral.srcAccessMask = {};
            toGeneral.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                      vk::AccessFlagBits::eShaderWrite |
                                      vk::AccessFlagBits::eTransferWrite;
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader |
                    vk::PipelineStageFlagBits::eComputeShader,
                {},
                {},
                {},
                toGeneral
            );
            commandBuffer.clearColorImage(
                *reservoir.image, vk::ImageLayout::eGeneral, clearZero, range
            );
        }
    }
    for (const auto &perImage : m_restirGiSpatialMetaPerImage) {
        for (const RestirMetaResources &meta : perImage) {
            vk::ImageMemoryBarrier toGeneral{};
            toGeneral.oldLayout = vk::ImageLayout::eUndefined;
            toGeneral.newLayout = vk::ImageLayout::eGeneral;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = *meta.image;
            toGeneral.subresourceRange = range;
            toGeneral.srcAccessMask = {};
            toGeneral.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                      vk::AccessFlagBits::eShaderWrite |
                                      vk::AccessFlagBits::eTransferWrite;
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader |
                    vk::PipelineStageFlagBits::eComputeShader,
                {},
                {},
                {},
                toGeneral
            );
            commandBuffer.clearColorImage(*meta.image, vk::ImageLayout::eGeneral, clearMeta, range);
        }
    }
    VulkanUtils::endSingleTimeCommands(
        device, m_context.getGraphicsQueue(), std::move(commandBuffer)
    );
}

void VulkanRenderer::cleanupRestirGiSpatialResources() {
    m_restirGiSpatialPipeline.clear();
    m_restirGiSpatialPipelineLayout.clear();
    m_restirGiSpatialDescriptorSets.clear();
    m_restirGiSpatialDescriptorPool.clear();
    m_restirGiSpatialDescriptorSetLayout.clear();
    m_restirGiSpatialPerImage.clear();
    m_restirGiSpatialMetaPerImage.clear();
}

void VulkanRenderer::updateRestirGiSpatialDescriptorSet(uint32_t imageIndex, uint32_t writeParity) {
    const uint32_t historyIndex = kRestirHistorySlot;
    if (imageIndex >= m_restirGiSpatialDescriptorSets.size() ||
        historyIndex >= m_restirGiPerImage.size() ||
        historyIndex >= m_restirGiMetaPerImage.size() ||
        historyIndex >= m_restirValidationPerImage.size() ||
        historyIndex >= m_restirGiSpatialPerImage.size() ||
        historyIndex >= m_restirGiSpatialMetaPerImage.size() || m_restirGiSampler == nullptr ||
        m_restirGiMetaSampler == nullptr || m_restirValidationSampler == nullptr) {
        return;
    }

    const uint32_t parity = writeParity & 1u;

    vk::DescriptorImageInfo temporalGiInfo{};
    temporalGiInfo.sampler = *m_restirGiSampler;
    temporalGiInfo.imageView = *m_restirGiPerImage[historyIndex][parity].view;
    temporalGiInfo.imageLayout = vk::ImageLayout::eGeneral;

    vk::DescriptorImageInfo temporalGiMetaInfo{};
    temporalGiMetaInfo.sampler = *m_restirGiMetaSampler;
    temporalGiMetaInfo.imageView = *m_restirGiMetaPerImage[historyIndex][parity].view;
    temporalGiMetaInfo.imageLayout = vk::ImageLayout::eGeneral;

    vk::DescriptorImageInfo temporalValidationInfo{};
    temporalValidationInfo.sampler = *m_restirValidationSampler;
    temporalValidationInfo.imageView = *m_restirValidationPerImage[historyIndex][parity].view;
    temporalValidationInfo.imageLayout = vk::ImageLayout::eGeneral;

    vk::DescriptorImageInfo spatialGiInfo{};
    spatialGiInfo.sampler = VK_NULL_HANDLE;
    spatialGiInfo.imageView = *m_restirGiSpatialPerImage[historyIndex][parity].view;
    spatialGiInfo.imageLayout = vk::ImageLayout::eGeneral;

    vk::DescriptorImageInfo spatialGiMetaInfo{};
    spatialGiMetaInfo.sampler = VK_NULL_HANDLE;
    spatialGiMetaInfo.imageView = *m_restirGiSpatialMetaPerImage[historyIndex][parity].view;
    spatialGiMetaInfo.imageLayout = vk::ImageLayout::eGeneral;

    std::array<vk::WriteDescriptorSet, 5> writes{};
    writes[0].dstSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    writes[0].dstBinding = 0;
    writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &temporalGiInfo;

    writes[1].dstSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    writes[1].dstBinding = 1;
    writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &temporalGiMetaInfo;

    writes[2].dstSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    writes[2].dstBinding = 2;
    writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &temporalValidationInfo;

    writes[3].dstSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    writes[3].dstBinding = 3;
    writes[3].descriptorType = vk::DescriptorType::eStorageImage;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &spatialGiInfo;

    writes[4].dstSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    writes[4].dstBinding = 4;
    writes[4].descriptorType = vk::DescriptorType::eStorageImage;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &spatialGiMetaInfo;

    const vk::raii::Device &device = m_context.getDevice();
    device.updateDescriptorSets(writes, {});
}

void VulkanRenderer::dispatchRestirGiSpatialPass(
    uint32_t imageIndex, const FrameRenderData &frameData
) {
    if (!kRestirGiDispatchSpatialPass) {
        return;
    }

    const uint32_t historyIndex = kRestirHistorySlot;
    if (!frameData.giLighting.pathTracingEnabled ||
        historyIndex >= m_restirDiWriteParityPerImage.size() ||
        imageIndex >= m_restirGiSpatialDescriptorSets.size() ||
        m_restirGiSpatialPipeline == nullptr || m_restirGiSpatialPipelineLayout == nullptr) {
        return;
    }

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0) {
        return;
    }

    const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
    updateRestirGiSpatialDescriptorSet(imageIndex, writeParity);

    if (historyIndex >= m_restirGiPerImage.size() ||
        historyIndex >= m_restirGiMetaPerImage.size() ||
        historyIndex >= m_restirValidationPerImage.size() ||
        historyIndex >= m_restirGiSpatialPerImage.size() ||
        historyIndex >= m_restirGiSpatialMetaPerImage.size()) {
        return;
    }

    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    std::array<vk::ImageMemoryBarrier, 5> barriers{};
    barriers[0].oldLayout = vk::ImageLayout::eGeneral;
    barriers[0].newLayout = vk::ImageLayout::eGeneral;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = *m_restirGiPerImage[historyIndex][writeParity].image;
    barriers[0].subresourceRange = range;
    barriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

    barriers[1].oldLayout = vk::ImageLayout::eGeneral;
    barriers[1].newLayout = vk::ImageLayout::eGeneral;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = *m_restirGiMetaPerImage[historyIndex][writeParity].image;
    barriers[1].subresourceRange = range;
    barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;

    barriers[2].oldLayout = vk::ImageLayout::eGeneral;
    barriers[2].newLayout = vk::ImageLayout::eGeneral;
    barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[2].image = *m_restirValidationPerImage[historyIndex][writeParity].image;
    barriers[2].subresourceRange = range;
    barriers[2].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[2].dstAccessMask = vk::AccessFlagBits::eShaderRead;

    barriers[3].oldLayout = vk::ImageLayout::eGeneral;
    barriers[3].newLayout = vk::ImageLayout::eGeneral;
    barriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[3].image = *m_restirGiSpatialPerImage[historyIndex][writeParity].image;
    barriers[3].subresourceRange = range;
    barriers[3].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[3].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

    barriers[4].oldLayout = vk::ImageLayout::eGeneral;
    barriers[4].newLayout = vk::ImageLayout::eGeneral;
    barriers[4].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[4].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[4].image = *m_restirGiSpatialMetaPerImage[historyIndex][writeParity].image;
    barriers[4].subresourceRange = range;
    barriers[4].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[4].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

    m_commandBuffers[imageIndex].pipelineBarrier(
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        {},
        {},
        barriers
    );

    m_commandBuffers[imageIndex].bindPipeline(
        vk::PipelineBindPoint::eCompute, *m_restirGiSpatialPipeline
    );
    const vk::DescriptorSet descriptorSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    m_commandBuffers[imageIndex].bindDescriptorSets(
        vk::PipelineBindPoint::eCompute, *m_restirGiSpatialPipelineLayout, 0, descriptorSet, {}
    );

    RestirGiSpatialPushConstants push{};
    push.extent = glm::uvec2(extent.width, extent.height);
    push.invExtent = glm::vec2(
        1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)
    );
    push.spatialWeight = glm::clamp(frameData.giLighting.restirSpatialReuse, 0.0f, 0.35f);
    push.lumaPhi = std::max(frameData.giLighting.denoiseLumaPhi, 0.05f);
    push.normalPower = 10.0f;
    push.depthScale = 24.0f;
    m_commandBuffers[imageIndex].pushConstants<RestirGiSpatialPushConstants>(
        *m_restirGiSpatialPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, push
    );

    const uint32_t groupCountX = (extent.width + 7u) / 8u;
    const uint32_t groupCountY = (extent.height + 7u) / 8u;
    if (groupCountX > 0 && groupCountY > 0) {
        m_commandBuffers[imageIndex].dispatch(groupCountX, groupCountY, 1u);
    }
}

void VulkanRenderer::clearTemporalGiWriteTargets(uint32_t imageIndex) {
    if (imageIndex >= m_commandBuffers.size()) {
        return;
    }

    struct ClearTarget {
        vk::Image image = VK_NULL_HANDLE;
        vk::ClearColorValue clear{};
    };

    const vk::ClearColorValue clearZero(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearValidation(std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.0f});
    const vk::ClearColorValue clearMeta(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearNormalRoughness(std::array<float, 4>{0.5f, 1.0f, 0.5f, 1.0f});
    const vk::ClearColorValue clearViewZ(std::array<float, 4>{-1.0f, 0.0f, 0.0f, 0.0f});

    std::vector<ClearTarget> targets;
    targets.reserve(9);
    auto addTarget = [&](vk::Image image, const vk::ClearColorValue &clearValue) {
        if (image == VK_NULL_HANDLE) {
            return;
        }
        targets.push_back(ClearTarget{image, clearValue});
    };

    const uint32_t historyIndex = kRestirHistorySlot;
    if (historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;

        if (historyIndex < m_restirDiPerImage.size()) {
            addTarget(*m_restirDiPerImage[historyIndex][writeParity].image, clearZero);
        }
        if (historyIndex < m_restirValidationPerImage.size()) {
            addTarget(
                *m_restirValidationPerImage[historyIndex][writeParity].image, clearValidation
            );
        }
        if (historyIndex < m_restirMetaPerImage.size()) {
            addTarget(*m_restirMetaPerImage[historyIndex][writeParity].image, clearMeta);
        }
        if (historyIndex < m_restirGiPerImage.size()) {
            addTarget(*m_restirGiPerImage[historyIndex][writeParity].image, clearZero);
        }
        if (historyIndex < m_restirGiMetaPerImage.size()) {
            addTarget(*m_restirGiMetaPerImage[historyIndex][writeParity].image, clearMeta);
        }
    }

    if (!m_nrdPerImage.empty()) {
        const NrdPerImageResources &nrd = m_nrdPerImage[0];
        addTarget(*nrd.diffIn.image, clearZero);
        addTarget(*nrd.normalRoughnessIn.image, clearNormalRoughness);
        addTarget(*nrd.motionIn.image, clearZero);
        addTarget(*nrd.viewZIn.image, clearViewZ);
    }

    if (targets.empty()) {
        return;
    }

    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    std::vector<vk::ImageMemoryBarrier> toTransfer;
    std::vector<vk::ImageMemoryBarrier> toShader;
    toTransfer.reserve(targets.size());
    toShader.reserve(targets.size());

    for (const ClearTarget &target : targets) {
        vk::ImageMemoryBarrier beforeClear{};
        beforeClear.oldLayout = vk::ImageLayout::eGeneral;
        beforeClear.newLayout = vk::ImageLayout::eGeneral;
        beforeClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        beforeClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        beforeClear.image = target.image;
        beforeClear.subresourceRange = range;
        beforeClear.srcAccessMask = vk::AccessFlagBits::eShaderRead |
                                    vk::AccessFlagBits::eShaderWrite |
                                    vk::AccessFlagBits::eTransferWrite;
        beforeClear.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        toTransfer.push_back(beforeClear);

        vk::ImageMemoryBarrier afterClear{};
        afterClear.oldLayout = vk::ImageLayout::eGeneral;
        afterClear.newLayout = vk::ImageLayout::eGeneral;
        afterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        afterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        afterClear.image = target.image;
        afterClear.subresourceRange = range;
        afterClear.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        afterClear.dstAccessMask =
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        toShader.push_back(afterClear);
    }

    m_commandBuffers[imageIndex].pipelineBarrier(
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        {},
        {},
        toTransfer
    );

    for (const ClearTarget &target : targets) {
        m_commandBuffers[imageIndex].clearColorImage(
            target.image, vk::ImageLayout::eGeneral, target.clear, range
        );
    }

    m_commandBuffers[imageIndex].pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
        {},
        {},
        {},
        toShader
    );
}
