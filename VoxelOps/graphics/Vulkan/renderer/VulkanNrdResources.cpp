#include "VulkanRenderer.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "../vulkan/VulkanUtils.hpp"
#include "NrdBootstrap.hpp"
#include "../graphics/Model.hpp"

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

#ifndef VOXELOPS_NRD_HEADERS
#define VOXELOPS_NRD_HEADERS 0
#endif

#if VOXELOPS_NRD_HEADERS
#include <NRD.h>
#endif

namespace {
#if VOXELOPS_NRD_HEADERS
    vk::raii::ShaderModule
    loadShaderModuleFromBytes(const vk::raii::Device &device, const void *bytes, size_t size) {
        if (bytes == nullptr || size == 0 || (size % 4) != 0) {
            throw std::runtime_error("Invalid NRD shader module bytecode.");
        }

        vk::ShaderModuleCreateInfo shaderModuleInfo{};
        shaderModuleInfo.codeSize = size;
        shaderModuleInfo.pCode = reinterpret_cast<const uint32_t *>(bytes);
        return vk::raii::ShaderModule(device, shaderModuleInfo);
    }

    vk::Format mapNrdFormatToVk(nrd::Format format) {
        switch (format) {
        case nrd::Format::R8_UNORM:
            return vk::Format::eR8Unorm;
        case nrd::Format::R8_SNORM:
            return vk::Format::eR8Snorm;
        case nrd::Format::R8_UINT:
            return vk::Format::eR8Uint;
        case nrd::Format::R8_SINT:
            return vk::Format::eR8Sint;
        case nrd::Format::RG8_UNORM:
            return vk::Format::eR8G8Unorm;
        case nrd::Format::RG8_SNORM:
            return vk::Format::eR8G8Snorm;
        case nrd::Format::RG8_UINT:
            return vk::Format::eR8G8Uint;
        case nrd::Format::RG8_SINT:
            return vk::Format::eR8G8Sint;
        case nrd::Format::RGBA8_UNORM:
            return vk::Format::eR8G8B8A8Unorm;
        case nrd::Format::RGBA8_SNORM:
            return vk::Format::eR8G8B8A8Snorm;
        case nrd::Format::RGBA8_UINT:
            return vk::Format::eR8G8B8A8Uint;
        case nrd::Format::RGBA8_SINT:
            return vk::Format::eR8G8B8A8Sint;
        case nrd::Format::RGBA8_SRGB:
            return vk::Format::eR8G8B8A8Srgb;
        case nrd::Format::R16_UNORM:
            return vk::Format::eR16Unorm;
        case nrd::Format::R16_SNORM:
            return vk::Format::eR16Snorm;
        case nrd::Format::R16_UINT:
            return vk::Format::eR16Uint;
        case nrd::Format::R16_SINT:
            return vk::Format::eR16Sint;
        case nrd::Format::R16_SFLOAT:
            return vk::Format::eR16Sfloat;
        case nrd::Format::RG16_UNORM:
            return vk::Format::eR16G16Unorm;
        case nrd::Format::RG16_SNORM:
            return vk::Format::eR16G16Snorm;
        case nrd::Format::RG16_UINT:
            return vk::Format::eR16G16Uint;
        case nrd::Format::RG16_SINT:
            return vk::Format::eR16G16Sint;
        case nrd::Format::RG16_SFLOAT:
            return vk::Format::eR16G16Sfloat;
        case nrd::Format::RGBA16_UNORM:
            return vk::Format::eR16G16B16A16Unorm;
        case nrd::Format::RGBA16_SNORM:
            return vk::Format::eR16G16B16A16Snorm;
        case nrd::Format::RGBA16_UINT:
            return vk::Format::eR16G16B16A16Uint;
        case nrd::Format::RGBA16_SINT:
            return vk::Format::eR16G16B16A16Sint;
        case nrd::Format::RGBA16_SFLOAT:
            return vk::Format::eR16G16B16A16Sfloat;
        case nrd::Format::R32_UINT:
            return vk::Format::eR32Uint;
        case nrd::Format::R32_SINT:
            return vk::Format::eR32Sint;
        case nrd::Format::R32_SFLOAT:
            return vk::Format::eR32Sfloat;
        case nrd::Format::RG32_UINT:
            return vk::Format::eR32G32Uint;
        case nrd::Format::RG32_SINT:
            return vk::Format::eR32G32Sint;
        case nrd::Format::RG32_SFLOAT:
            return vk::Format::eR32G32Sfloat;
        case nrd::Format::RGB32_UINT:
            return vk::Format::eR32G32B32Uint;
        case nrd::Format::RGB32_SINT:
            return vk::Format::eR32G32B32Sint;
        case nrd::Format::RGB32_SFLOAT:
            return vk::Format::eR32G32B32Sfloat;
        case nrd::Format::RGBA32_UINT:
            return vk::Format::eR32G32B32A32Uint;
        case nrd::Format::RGBA32_SINT:
            return vk::Format::eR32G32B32A32Sint;
        case nrd::Format::RGBA32_SFLOAT:
            return vk::Format::eR32G32B32A32Sfloat;
        case nrd::Format::R10_G10_B10_A2_UNORM:
            return vk::Format::eA2B10G10R10UnormPack32;
        case nrd::Format::R10_G10_B10_A2_UINT:
            return vk::Format::eA2B10G10R10UintPack32;
        case nrd::Format::R11_G11_B10_UFLOAT:
            return vk::Format::eB10G11R11UfloatPack32;
        case nrd::Format::R9_G9_B9_E5_UFLOAT:
            return vk::Format::eE5B9G9R9UfloatPack32;
        default:
            break;
        }
        return vk::Format::eUndefined;
    } // namespace

    uint32_t divideUp(uint32_t x, uint16_t y) {
        return (x + static_cast<uint32_t>(y) - 1u) / static_cast<uint32_t>(y);
    }
#endif

    vk::Format chooseNrdNormalRoughnessFormat(uint32_t normalEncoding) {
        switch (normalEncoding) {
        case 0u: // nrd::NormalEncoding::RGBA8_UNORM
            return vk::Format::eR8G8B8A8Unorm;
        case 1u: // nrd::NormalEncoding::RGBA8_SNORM
            return vk::Format::eR8G8B8A8Snorm;
        case 2u: // nrd::NormalEncoding::R10_G10_B10_A2_UNORM
            return vk::Format::eA2B10G10R10UnormPack32;
        case 3u: // nrd::NormalEncoding::RGBA16_UNORM
            return vk::Format::eR16G16B16A16Unorm;
        case 4u: // nrd::NormalEncoding::RGBA16_SNORM
            return vk::Format::eR16G16B16A16Snorm;
        default:
            break;
        }
        return vk::Format::eR16G16B16A16Sfloat;
    }

    vk::ClearColorValue nrdNormalRoughnessClearValue(uint32_t normalEncoding) {
        if (normalEncoding == 2u) {
            // Packed oct normal for +Z and roughness=1, materialId=0.
            return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 0.0f});
        }
        if (normalEncoding == 1u || normalEncoding == 4u) {
            return vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
        }
        return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f});
    }
} // namespace

void VulkanRenderer::createNrdSignalResources() {
    cleanupNrdSignalResources();

    if (m_commandPool == nullptr) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();
    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0) {
        return;
    }

    const size_t imageCount = m_context.getSwapchainImageViews().size();
    if (imageCount == 0) {
        return;
    }
    m_nrdPerImage.resize(imageCount);
    m_nrdValidPerImage.assign(imageCount, false);

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst |
                                           vk::ImageUsageFlagBits::eColorAttachment;
    constexpr vk::Format kDiffFormat = vk::Format::eR16G16B16A16Sfloat;
    const uint32_t normalEncoding =
        (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->normalEncoding() : 2u;
    const vk::Format kNormalRoughnessFormat = chooseNrdNormalRoughnessFormat(normalEncoding);
    constexpr vk::Format kMotionFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kViewZFormat = vk::Format::eR32Sfloat;
    constexpr vk::Format kShadowPenumbraFormat = vk::Format::eR16Sfloat;
    constexpr vk::Format kShadowOutFormat = vk::Format::eR8Unorm;
    constexpr vk::Format kComposeFormat = vk::Format::eR16G16B16A16Sfloat;
    m_loggedMissingNrdResources = false;

    if (m_nrdOutputSampler == nullptr) {
        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = vk::CompareOp::eAlways;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        m_nrdOutputSampler = vk::raii::Sampler(device, samplerInfo);
    }

    const auto supportsStorage = [&](vk::Format format) -> bool {
        const vk::FormatProperties props = physicalDevice.getFormatProperties(format);
        return static_cast<bool>(
            props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage
        );
    };
    if (!supportsStorage(kDiffFormat) || !supportsStorage(kNormalRoughnessFormat) ||
        !supportsStorage(kMotionFormat) || !supportsStorage(kViewZFormat) ||
        !supportsStorage(kShadowPenumbraFormat) || !supportsStorage(kShadowOutFormat) ||
        !supportsStorage(kComposeFormat)) {
        throw std::runtime_error(
            "NRD signal formats are not supported as storage images by this Vulkan device."
        );
    }

    const auto createSignal = [&](SignalImageResources &dst,
                                  vk::Format format,
                                  uint32_t w,
                                  uint32_t h) {
        vk::ImageCreateInfo imageInfo{};
        imageInfo.imageType = vk::ImageType::e2D;
        imageInfo.extent = vk::Extent3D(w, h, 1u);
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = vk::ImageTiling::eOptimal;
        imageInfo.initialLayout = vk::ImageLayout::eUndefined;
        imageInfo.usage = kUsage;
        imageInfo.samples = vk::SampleCountFlagBits::e1;
        imageInfo.sharingMode = vk::SharingMode::eExclusive;
        dst.image = vk::raii::Image(device, imageInfo);

        const vk::MemoryRequirements requirements = dst.image.getMemoryRequirements();
        vk::MemoryAllocateInfo allocInfo{};
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = VulkanUtils::findMemoryType(
            physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal
        );
        dst.memory = vk::raii::DeviceMemory(device, allocInfo);
        dst.image.bindMemory(*dst.memory, 0);

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = *dst.image;
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        dst.view = vk::raii::ImageView(device, viewInfo);
    };

    createSignal(m_nrdFallback.diffIn, kDiffFormat, 1u, 1u);
    createSignal(m_nrdFallback.normalRoughnessIn, kNormalRoughnessFormat, 1u, 1u);
    createSignal(m_nrdFallback.motionIn, kMotionFormat, 1u, 1u);
    createSignal(m_nrdFallback.viewZIn, kViewZFormat, 1u, 1u);
    createSignal(m_nrdFallback.diffOut, kDiffFormat, 1u, 1u);
    createSignal(m_nrdFallback.shadowIn, kShadowPenumbraFormat, 1u, 1u);
    createSignal(m_nrdFallback.shadowOut, kShadowOutFormat, 1u, 1u);
    createSignal(m_nrdFallback.composeBase, kComposeFormat, 1u, 1u);
    createSignal(m_nrdFallback.composeIndirect, kComposeFormat, 1u, 1u);
    m_nrdFallbackReady = true;

    for (NrdPerImageResources &resources : m_nrdPerImage) {
        createSignal(resources.diffIn, kDiffFormat, extent.width, extent.height);
        createSignal(resources.normalRoughnessIn, kNormalRoughnessFormat, extent.width, extent.height);
        createSignal(resources.motionIn, kMotionFormat, extent.width, extent.height);
        createSignal(resources.viewZIn, kViewZFormat, extent.width, extent.height);
        createSignal(resources.diffOut, kDiffFormat, extent.width, extent.height);
        createSignal(resources.shadowIn, kShadowPenumbraFormat, extent.width, extent.height);
        createSignal(resources.shadowOut, kShadowOutFormat, extent.width, extent.height);
        createSignal(resources.composeBase, kComposeFormat, extent.width, extent.height);
        createSignal(resources.composeIndirect, kComposeFormat, extent.width, extent.height);
    }

    vk::raii::CommandBuffer commandBuffer =
        VulkanUtils::beginSingleTimeCommands(device, m_commandPool);
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    const auto transitionAndClear = [&](vk::Image image, const vk::ClearColorValue &clearValue) {
        vk::ImageMemoryBarrier toGeneral{};
        toGeneral.oldLayout = vk::ImageLayout::eUndefined;
        toGeneral.newLayout = vk::ImageLayout::eGeneral;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = image;
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
        commandBuffer.clearColorImage(image, vk::ImageLayout::eGeneral, clearValue, range);
    };

    const vk::ClearColorValue clearZero(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearNormalRoughness = nrdNormalRoughnessClearValue(normalEncoding);
    const vk::ClearColorValue clearViewZ(std::array<float, 4>{-1.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearPenumbraLit(std::array<float, 4>{65504.0f, 0.0f, 0.0f, 0.0f});
    transitionAndClear(*m_nrdFallback.diffIn.image, clearZero);
    transitionAndClear(*m_nrdFallback.normalRoughnessIn.image, clearNormalRoughness);
    transitionAndClear(*m_nrdFallback.motionIn.image, clearZero);
    transitionAndClear(*m_nrdFallback.viewZIn.image, clearViewZ);
    transitionAndClear(*m_nrdFallback.diffOut.image, clearZero);
    transitionAndClear(*m_nrdFallback.shadowIn.image, clearPenumbraLit);
    transitionAndClear(*m_nrdFallback.shadowOut.image, clearZero);
    transitionAndClear(*m_nrdFallback.composeBase.image, clearZero);
    transitionAndClear(*m_nrdFallback.composeIndirect.image, clearZero);
    for (const NrdPerImageResources &resources : m_nrdPerImage) {
        transitionAndClear(*resources.diffIn.image, clearZero);
        transitionAndClear(*resources.normalRoughnessIn.image, clearNormalRoughness);
        transitionAndClear(*resources.motionIn.image, clearZero);
        transitionAndClear(*resources.viewZIn.image, clearViewZ);
        transitionAndClear(*resources.diffOut.image, clearZero);
        transitionAndClear(*resources.shadowIn.image, clearPenumbraLit);
        transitionAndClear(*resources.shadowOut.image, clearZero);
        transitionAndClear(*resources.composeBase.image, clearZero);
        transitionAndClear(*resources.composeIndirect.image, clearZero);
    }

    VulkanUtils::endSingleTimeCommands(
        device, m_context.getGraphicsQueue(), std::move(commandBuffer)
    );
}

void VulkanRenderer::cleanupNrdSignalResources() {
    m_nrdPerImage.clear();
    m_nrdValidPerImage.clear();
    m_nrdFallback.diffIn.view.clear();
    m_nrdFallback.diffIn.image.clear();
    m_nrdFallback.diffIn.memory.clear();
    m_nrdFallback.normalRoughnessIn.view.clear();
    m_nrdFallback.normalRoughnessIn.image.clear();
    m_nrdFallback.normalRoughnessIn.memory.clear();
    m_nrdFallback.motionIn.view.clear();
    m_nrdFallback.motionIn.image.clear();
    m_nrdFallback.motionIn.memory.clear();
    m_nrdFallback.viewZIn.view.clear();
    m_nrdFallback.viewZIn.image.clear();
    m_nrdFallback.viewZIn.memory.clear();
    m_nrdFallback.diffOut.view.clear();
    m_nrdFallback.diffOut.image.clear();
    m_nrdFallback.diffOut.memory.clear();
    m_nrdFallback.shadowIn.view.clear();
    m_nrdFallback.shadowIn.image.clear();
    m_nrdFallback.shadowIn.memory.clear();
    m_nrdFallback.shadowOut.view.clear();
    m_nrdFallback.shadowOut.image.clear();
    m_nrdFallback.shadowOut.memory.clear();
    m_nrdFallback.composeBase.view.clear();
    m_nrdFallback.composeBase.image.clear();
    m_nrdFallback.composeBase.memory.clear();
    m_nrdFallback.composeIndirect.view.clear();
    m_nrdFallback.composeIndirect.image.clear();
    m_nrdFallback.composeIndirect.memory.clear();
    m_nrdOutputSampler.clear();
    m_nrdFallbackReady = false;
    m_loggedMissingNrdResources = false;
    m_nrdPrevViewProjection = glm::mat4(1.0f);
    m_nrdPrevView = glm::mat4(1.0f);
    m_nrdPrevProjection = glm::mat4(1.0f);
    m_nrdPrevMatricesValid = false;
}

#if VOXELOPS_NRD_HEADERS
bool VulkanRenderer::createNrdRuntimeResources() {
    cleanupNrdRuntimeResources();

    try {
        if (m_nrdBootstrap == nullptr || !m_nrdBootstrap->isActive()) {
            return false;
        }
        const nrd::LibraryDesc *libraryDesc =
            static_cast<const nrd::LibraryDesc *>(m_nrdBootstrap->libraryDescData());
        const nrd::InstanceDesc *instanceDesc =
            static_cast<const nrd::InstanceDesc *>(m_nrdBootstrap->instanceDescData());
        if (libraryDesc == nullptr || instanceDesc == nullptr || m_framebuffers.empty()) {
            return false;
        }

        if (instanceDesc->pipelines == nullptr || instanceDesc->pipelinesNum == 0 ||
            instanceDesc->constantBufferMaxDataSize == 0) {
            return false;
        }

        if (!m_context.isComputeShaderDerivativesEnabled()) {
            std::cerr << "[Vulkan][NRD] Runtime disabled: VK_KHR_compute_shader_derivatives "
                      << "with computeDerivativeGroupQuads is unavailable.\n";
            return false;
        }

        const vk::raii::Device &device = m_context.getDevice();
        const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();
        const vk::Extent2D extent = m_context.getSwapchainExtent();
        if (extent.width == 0 || extent.height == 0) {
            return false;
        }

        m_nrdTextureBinding = libraryDesc->spirvBindingOffsets.textureOffset +
                              instanceDesc->resourcesBaseRegisterIndex;
        m_nrdStorageBinding = libraryDesc->spirvBindingOffsets.storageTextureAndBufferOffset +
                              instanceDesc->resourcesBaseRegisterIndex;
        m_nrdConstantBinding = libraryDesc->spirvBindingOffsets.constantBufferOffset +
                               instanceDesc->constantBufferRegisterIndex;
        m_nrdSetResourcesIndex = instanceDesc->resourcesSpaceIndex;
        m_nrdSetConstantsIndex = instanceDesc->constantBufferAndSamplersSpaceIndex;
        m_nrdTextureCapacity = std::max(1u, instanceDesc->descriptorPoolDesc.perSetTexturesMaxNum);
        m_nrdStorageCapacity =
            std::max(1u, instanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum);
        m_nrdConstantBufferSize = instanceDesc->constantBufferMaxDataSize;

        const vk::PhysicalDeviceProperties properties = physicalDevice.getProperties();
        const vk::DeviceSize uboAlignment =
            std::max<vk::DeviceSize>(16, properties.limits.minUniformBufferOffsetAlignment);
        const vk::DeviceSize alignedCbSize =
            ((static_cast<vk::DeviceSize>(m_nrdConstantBufferSize) + uboAlignment - 1) /
             uboAlignment) *
            uboAlignment;
        m_nrdConstantBufferStride = static_cast<uint32_t>(alignedCbSize);

        {
            std::vector<vk::DescriptorSetLayoutBinding> bindings;
            bindings.reserve(
                static_cast<size_t>(m_nrdTextureCapacity) +
                static_cast<size_t>(m_nrdStorageCapacity)
            );

            const auto pushBinding = [&](uint32_t binding, vk::DescriptorType type) {
                for (const vk::DescriptorSetLayoutBinding &existing : bindings) {
                    if (existing.binding != binding) {
                        continue;
                    }
                    if (existing.descriptorType != type) {
                        throw std::runtime_error(
                            "NRD descriptor binding collision in resources set layout."
                        );
                    }
                    return;
                }

                vk::DescriptorSetLayoutBinding layoutBinding{};
                layoutBinding.binding = binding;
                layoutBinding.descriptorType = type;
                layoutBinding.descriptorCount = 1;
                layoutBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;
                bindings.push_back(layoutBinding);
            };

            for (uint32_t i = 0; i < m_nrdTextureCapacity; ++i) {
                pushBinding(m_nrdTextureBinding + i, vk::DescriptorType::eSampledImage);
            }
            for (uint32_t i = 0; i < m_nrdStorageCapacity; ++i) {
                pushBinding(m_nrdStorageBinding + i, vk::DescriptorType::eStorageImage);
            }
            std::sort(bindings.begin(), bindings.end(), [](const auto &a, const auto &b) {
                return a.binding < b.binding;
            });

            vk::DescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();
            m_nrdResourcesSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);
        }

        if (m_nrdNearestSampler == nullptr) {
            vk::SamplerCreateInfo samplerInfo{};
            samplerInfo.magFilter = vk::Filter::eNearest;
            samplerInfo.minFilter = vk::Filter::eNearest;
            samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
            samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            samplerInfo.mipLodBias = 0.0f;
            samplerInfo.anisotropyEnable = VK_FALSE;
            samplerInfo.maxAnisotropy = 1.0f;
            samplerInfo.compareEnable = VK_FALSE;
            samplerInfo.compareOp = vk::CompareOp::eAlways;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = 0.0f;
            samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
            samplerInfo.unnormalizedCoordinates = VK_FALSE;
            m_nrdNearestSampler = vk::raii::Sampler(device, samplerInfo);
        }
        if (m_nrdLinearSampler == nullptr) {
            vk::SamplerCreateInfo samplerInfo{};
            samplerInfo.magFilter = vk::Filter::eLinear;
            samplerInfo.minFilter = vk::Filter::eLinear;
            samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
            samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            samplerInfo.mipLodBias = 0.0f;
            samplerInfo.anisotropyEnable = VK_FALSE;
            samplerInfo.maxAnisotropy = 1.0f;
            samplerInfo.compareEnable = VK_FALSE;
            samplerInfo.compareOp = vk::CompareOp::eAlways;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = 0.0f;
            samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
            samplerInfo.unnormalizedCoordinates = VK_FALSE;
            m_nrdLinearSampler = vk::raii::Sampler(device, samplerInfo);
        }

        {
            std::vector<vk::DescriptorSetLayoutBinding> bindings;
            vk::DescriptorSetLayoutBinding constantBinding{};
            constantBinding.binding = m_nrdConstantBinding;
            constantBinding.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
            constantBinding.descriptorCount = 1;
            constantBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;
            bindings.push_back(constantBinding);

            for (uint32_t i = 0; i < instanceDesc->samplersNum; ++i) {
                vk::DescriptorSetLayoutBinding samplerBinding{};
                samplerBinding.binding = libraryDesc->spirvBindingOffsets.samplerOffset +
                                         instanceDesc->samplersBaseRegisterIndex + i;
                samplerBinding.descriptorType = vk::DescriptorType::eSampler;
                samplerBinding.descriptorCount = 1;
                samplerBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;
                bindings.push_back(samplerBinding);
            }

            vk::DescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();
            m_nrdConstantsSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);
        }

        vk::raii::DescriptorSetLayout emptySetLayout{nullptr};
        const uint32_t maxSetIndex = std::max(m_nrdSetResourcesIndex, m_nrdSetConstantsIndex);
        std::vector<vk::DescriptorSetLayout> setLayouts(maxSetIndex + 1u, VK_NULL_HANDLE);
        setLayouts[m_nrdSetResourcesIndex] = *m_nrdResourcesSetLayout;
        setLayouts[m_nrdSetConstantsIndex] = *m_nrdConstantsSetLayout;
        for (uint32_t i = 0; i < setLayouts.size(); ++i) {
            if (setLayouts[i] == VK_NULL_HANDLE) {
                if (emptySetLayout == nullptr) {
                    vk::DescriptorSetLayoutCreateInfo emptyInfo{};
                    emptySetLayout = vk::raii::DescriptorSetLayout(device, emptyInfo);
                }
                setLayouts[i] = *emptySetLayout;
            }
        }

        vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        m_nrdPipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

        m_nrdPipelines.reserve(instanceDesc->pipelinesNum);
        for (uint32_t i = 0; i < instanceDesc->pipelinesNum; ++i) {
            const nrd::PipelineDesc &pipelineDesc = instanceDesc->pipelines[i];
            if (pipelineDesc.computeShaderSPIRV.bytecode == nullptr ||
                pipelineDesc.computeShaderSPIRV.size == 0) {
                throw std::runtime_error("NRD pipeline has missing SPIR-V bytecode.");
            }

            vk::raii::ShaderModule computeShader = loadShaderModuleFromBytes(
                device,
                pipelineDesc.computeShaderSPIRV.bytecode,
                static_cast<size_t>(pipelineDesc.computeShaderSPIRV.size)
            );
            vk::PipelineShaderStageCreateInfo stageInfo{};
            stageInfo.stage = vk::ShaderStageFlagBits::eCompute;
            stageInfo.module = *computeShader;
            stageInfo.pName = (instanceDesc->shaderEntryPoint != nullptr)
                                  ? instanceDesc->shaderEntryPoint
                                  : "main";

            vk::ComputePipelineCreateInfo computeInfo{};
            computeInfo.stage = stageInfo;
            computeInfo.layout = *m_nrdPipelineLayout;
            m_nrdPipelines.emplace_back(device, nullptr, computeInfo);
        }

        auto createRuntimeTexture = [&](NrdRuntimeTexture &dst, const nrd::TextureDesc &src) {
            const vk::Format format = mapNrdFormatToVk(src.format);
            if (format == vk::Format::eUndefined) {
                throw std::runtime_error("Unsupported NRD texture format in Vulkan mapping.");
            }
            const std::string formatId = std::to_string(static_cast<uint32_t>(format));
            const vk::FormatProperties props = physicalDevice.getFormatProperties(format);
            if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) ==
                vk::FormatFeatureFlags()) {
                throw std::runtime_error(
                    "NRD sampled format " + formatId + " is not supported by this Vulkan device."
                );
            }
            if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage) ==
                vk::FormatFeatureFlags()) {
                throw std::runtime_error(
                    "NRD storage format " + formatId + " is not supported by this Vulkan device."
                );
            }

            dst.format = format;
            dst.width = std::max(1u, divideUp(extent.width, src.downsampleFactor));
            dst.height = std::max(1u, divideUp(extent.height, src.downsampleFactor));

            vk::ImageCreateInfo imageInfo{};
            imageInfo.imageType = vk::ImageType::e2D;
            imageInfo.extent = vk::Extent3D(dst.width, dst.height, 1u);
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = format;
            imageInfo.tiling = vk::ImageTiling::eOptimal;
            imageInfo.initialLayout = vk::ImageLayout::eUndefined;
            imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
            imageInfo.samples = vk::SampleCountFlagBits::e1;
            imageInfo.sharingMode = vk::SharingMode::eExclusive;
            dst.image.image = vk::raii::Image(device, imageInfo);

            const vk::MemoryRequirements requirements = dst.image.image.getMemoryRequirements();
            vk::MemoryAllocateInfo allocInfo{};
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex = VulkanUtils::findMemoryType(
                physicalDevice,
                requirements.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eDeviceLocal
            );
            dst.image.memory = vk::raii::DeviceMemory(device, allocInfo);
            dst.image.image.bindMemory(*dst.image.memory, 0);

            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = *dst.image.image;
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            dst.image.view = vk::raii::ImageView(device, viewInfo);
        };

        // NRD history must persist in frame order, not swapchain-image order.
        // Keep a single shared history pool and per-frame descriptor resources.
        m_nrdRuntimePerImage.resize(1);
        m_nrdRuntimePerFrame.resize(MAX_FRAMES_IN_FLIGHT);
        const uint32_t maxDispatchSets = std::max(
            64u, std::max(instanceDesc->descriptorPoolDesc.setsMaxNum, instanceDesc->pipelinesNum)
        );
        const uint32_t maxConstantsSets = maxDispatchSets;
        const vk::DeviceSize constantBufferBytes =
            static_cast<vk::DeviceSize>(m_nrdConstantBufferStride) *
            static_cast<vk::DeviceSize>(maxConstantsSets);

        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, m_commandPool);
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

        auto transitionPool = [&](std::vector<NrdRuntimeTexture> &pool) {
            for (NrdRuntimeTexture &tex : pool) {
                vk::ImageMemoryBarrier barrier{};
                barrier.oldLayout = vk::ImageLayout::eUndefined;
                barrier.newLayout = vk::ImageLayout::eGeneral;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = *tex.image.image;
                barrier.subresourceRange = range;
                barrier.srcAccessMask = {};
                barrier.dstAccessMask =
                    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTopOfPipe,
                    vk::PipelineStageFlagBits::eComputeShader,
                    {},
                    {},
                    {},
                    barrier
                );
            }
        };

        NrdRuntimePerImage &sharedRuntime = m_nrdRuntimePerImage[0];
        sharedRuntime.permanentPool.resize(instanceDesc->permanentPoolSize);
        sharedRuntime.transientPool.resize(instanceDesc->transientPoolSize);
        for (uint32_t i = 0; i < instanceDesc->permanentPoolSize; ++i) {
            createRuntimeTexture(sharedRuntime.permanentPool[i], instanceDesc->permanentPool[i]);
        }
        for (uint32_t i = 0; i < instanceDesc->transientPoolSize; ++i) {
            createRuntimeTexture(sharedRuntime.transientPool[i], instanceDesc->transientPool[i]);
        }
        transitionPool(sharedRuntime.permanentPool);
        transitionPool(sharedRuntime.transientPool);

        for (NrdRuntimePerFrame &runtimeFrame : m_nrdRuntimePerFrame) {
            VulkanUtils::createBuffer(
                device,
                physicalDevice,
                constantBufferBytes,
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                runtimeFrame.constantBuffer,
                runtimeFrame.constantMemory
            );
            runtimeFrame.constantMapped = runtimeFrame.constantMemory.mapMemory(0, constantBufferBytes);

            std::array<vk::DescriptorPoolSize, 4> poolSizes{};
            poolSizes[0].type = vk::DescriptorType::eSampledImage;
            poolSizes[0].descriptorCount = m_nrdTextureCapacity * maxDispatchSets;
            poolSizes[1].type = vk::DescriptorType::eStorageImage;
            poolSizes[1].descriptorCount = m_nrdStorageCapacity * maxDispatchSets;
            poolSizes[2].type = vk::DescriptorType::eUniformBufferDynamic;
            poolSizes[2].descriptorCount = 1;
            poolSizes[3].type = vk::DescriptorType::eSampler;
            poolSizes[3].descriptorCount = instanceDesc->samplersNum;

            vk::DescriptorPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
            poolInfo.maxSets = maxDispatchSets + 1u;
            poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            runtimeFrame.descriptorPool = vk::raii::DescriptorPool(device, poolInfo);

            std::vector<vk::DescriptorSetLayout> layouts(
                maxDispatchSets + 1u, *m_nrdResourcesSetLayout
            );
            layouts[maxDispatchSets] = *m_nrdConstantsSetLayout;
            vk::DescriptorSetAllocateInfo allocInfo{};
            allocInfo.descriptorPool = *runtimeFrame.descriptorPool;
            allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
            allocInfo.pSetLayouts = layouts.data();
            std::vector<vk::raii::DescriptorSet> sets = device.allocateDescriptorSets(allocInfo);
            runtimeFrame.resourcesSets.clear();
            runtimeFrame.resourcesSets.reserve(maxDispatchSets);
            for (uint32_t setIndex = 0; setIndex < maxDispatchSets; ++setIndex) {
                runtimeFrame.resourcesSets.push_back(std::move(sets[setIndex]));
            }
            runtimeFrame.constantsSet = std::move(sets[maxDispatchSets]);

            vk::DescriptorBufferInfo constantInfo{};
            constantInfo.buffer = *runtimeFrame.constantBuffer;
            constantInfo.offset = 0;
            constantInfo.range = m_nrdConstantBufferSize;
            vk::WriteDescriptorSet constantWrite{};
            constantWrite.dstSet = *runtimeFrame.constantsSet;
            constantWrite.dstBinding = m_nrdConstantBinding;
            constantWrite.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
            constantWrite.descriptorCount = 1;
            constantWrite.pBufferInfo = &constantInfo;

            std::vector<vk::DescriptorImageInfo> samplerInfos(instanceDesc->samplersNum);
            std::vector<vk::WriteDescriptorSet> samplerWrites(instanceDesc->samplersNum);
            for (uint32_t i = 0; i < instanceDesc->samplersNum; ++i) {
                const bool linear = instanceDesc->samplers[i] == nrd::Sampler::LINEAR_CLAMP;
                samplerInfos[i].sampler = linear ? *m_nrdLinearSampler : *m_nrdNearestSampler;
                samplerInfos[i].imageView = VK_NULL_HANDLE;
                samplerInfos[i].imageLayout = vk::ImageLayout::eUndefined;

                samplerWrites[i].dstSet = *runtimeFrame.constantsSet;
                samplerWrites[i].dstBinding = libraryDesc->spirvBindingOffsets.samplerOffset +
                                              instanceDesc->samplersBaseRegisterIndex + i;
                samplerWrites[i].descriptorType = vk::DescriptorType::eSampler;
                samplerWrites[i].descriptorCount = 1;
                samplerWrites[i].pImageInfo = &samplerInfos[i];
            }

            std::vector<vk::WriteDescriptorSet> writes;
            writes.reserve(1u + samplerWrites.size());
            writes.push_back(constantWrite);
            writes.insert(writes.end(), samplerWrites.begin(), samplerWrites.end());
            device.updateDescriptorSets(writes, {});
        }

        VulkanUtils::endSingleTimeCommands(
            device, m_context.getGraphicsQueue(), std::move(commandBuffer)
        );

        m_nrdRuntimeReady = true;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][NRD] Runtime initialization failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[Vulkan][NRD] Runtime initialization failed: unknown exception.\n";
    }

    cleanupNrdRuntimeResources();
    return false;
}

void VulkanRenderer::cleanupNrdRuntimeResources() {
    for (NrdRuntimePerImage &runtime : m_nrdRuntimePerImage) {
        runtime.permanentPool.clear();
        runtime.transientPool.clear();
    }
    m_nrdRuntimePerImage.clear();
    for (NrdRuntimePerFrame &runtimeFrame : m_nrdRuntimePerFrame) {
        if (runtimeFrame.constantMapped != nullptr) {
            runtimeFrame.constantMemory.unmapMemory();
            runtimeFrame.constantMapped = nullptr;
        }
        runtimeFrame.resourcesSets.clear();
        runtimeFrame.constantsSet.clear();
        runtimeFrame.descriptorPool.clear();
        runtimeFrame.constantBuffer.clear();
        runtimeFrame.constantMemory.clear();
    }
    m_nrdRuntimePerFrame.clear();
    m_nrdPipelines.clear();
    m_nrdPipelineLayout.clear();
    m_nrdResourcesSetLayout.clear();
    m_nrdConstantsSetLayout.clear();
    m_nrdNearestSampler.clear();
    m_nrdLinearSampler.clear();
    m_nrdTextureBinding = 0;
    m_nrdStorageBinding = 0;
    m_nrdConstantBinding = 0;
    m_nrdSetResourcesIndex = 0;
    m_nrdSetConstantsIndex = 0;
    m_nrdTextureCapacity = 0;
    m_nrdStorageCapacity = 0;
    m_nrdConstantBufferStride = 0;
    m_nrdConstantBufferSize = 0;
    m_nrdRuntimeReady = false;
}

void VulkanRenderer::dispatchNrdPass(
    vk::CommandBuffer commandBuffer, uint32_t imageIndex, const FrameRenderData &frameData
) {
    const auto setNrdValidForAllImages = [&](bool valid) {
        std::fill(m_nrdValidPerImage.begin(), m_nrdValidPerImage.end(), valid);
    };
    const auto setNrdValidForImage = [&](bool valid) {
        // NRD runtime history is shared across all swapchain images, so validity is global.
        setNrdValidForAllImages(valid);
    };

    if (!m_nrdRuntimeReady || m_nrdBootstrap == nullptr || !m_nrdBootstrap->isActive() ||
        m_nrdRuntimePerImage.empty() || m_nrdRuntimePerFrame.empty() || m_nrdPerImage.empty()) {
        setNrdValidForAllImages(false);
        return;
    }
    if (!frameData.giLighting.pathTracingEnabled || frameData.giLighting.resetHistory) {
        setNrdValidForAllImages(false);
    }
    if (!frameData.giLighting.pathTracingEnabled) {
        return;
    }

    const nrd::InstanceDesc *instanceDesc =
        static_cast<const nrd::InstanceDesc *>(m_nrdBootstrap->instanceDescData());
    const nrd::DispatchDesc *dispatches =
        static_cast<const nrd::DispatchDesc *>(m_nrdBootstrap->dispatchDescData());
    const uint32_t dispatchCount = m_nrdBootstrap->lastDispatchCount();
    if (instanceDesc == nullptr || dispatches == nullptr || dispatchCount == 0) {
        setNrdValidForImage(false);
        return;
    }

    const size_t runtimeIndex = 0u;
    const size_t frameRuntimeIndex =
        std::min<size_t>(m_frameSync.getCurrentFrame(), m_nrdRuntimePerFrame.size() - 1u);
    const size_t externalIndex = std::min<size_t>(imageIndex, m_nrdPerImage.size() - 1u);
    NrdRuntimePerImage &runtime = m_nrdRuntimePerImage[runtimeIndex];
    NrdRuntimePerFrame &runtimeFrame = m_nrdRuntimePerFrame[frameRuntimeIndex];
    const NrdPerImageResources &external = m_nrdPerImage[externalIndex];
    if (runtimeFrame.constantMapped == nullptr || runtimeFrame.constantsSet == nullptr ||
        runtimeFrame.resourcesSets.empty()) {
        setNrdValidForImage(false);
        return;
    }

    const auto getExternalView = [&](nrd::ResourceType type, vk::ImageView &outView) -> bool {
        switch (type) {
        case nrd::ResourceType::IN_MV:
            outView = *external.motionIn.view;
            return true;
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
            outView = *external.normalRoughnessIn.view;
            return true;
        case nrd::ResourceType::IN_VIEWZ:
            outView = *external.viewZIn.view;
            return true;
        case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
            outView = *external.diffIn.view;
            return true;
        case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
            outView = *external.diffOut.view;
            return true;
        case nrd::ResourceType::IN_PENUMBRA:
            outView = *external.shadowIn.view;
            return true;
        case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
            outView = *external.shadowOut.view;
            return true;
        default:
            break;
        }
        return false;
    };

    std::vector<vk::DescriptorImageInfo> sampledInfos(m_nrdTextureCapacity);
    std::vector<vk::DescriptorImageInfo> storageInfos(m_nrdStorageCapacity);
    vk::ImageView sampledFallback = *external.normalRoughnessIn.view;
    vk::ImageView storageFallback = *external.diffOut.view;
    for (vk::DescriptorImageInfo &info : sampledInfos) {
        info.sampler = VK_NULL_HANDLE;
        info.imageView = sampledFallback;
        info.imageLayout = vk::ImageLayout::eGeneral;
    }
    for (vk::DescriptorImageInfo &info : storageInfos) {
        info.sampler = VK_NULL_HANDLE;
        info.imageView = storageFallback;
        info.imageLayout = vk::ImageLayout::eGeneral;
    }

    const uint32_t maxConstantsSets = static_cast<uint32_t>(runtimeFrame.resourcesSets.size());
    uint32_t constantSetIndex = 0;
    bool loggedResourceSetOverflow = false;
    bool dispatchedAll = true;

    const vk::PipelineLayout pipelineLayout = *m_nrdPipelineLayout;

    for (uint32_t i = 0; i < dispatchCount; ++i) {
        if (i >= runtimeFrame.resourcesSets.size()) {
            if (!loggedResourceSetOverflow) {
                std::cerr << "[Vulkan][NRD] Insufficient per-dispatch descriptor sets: required="
                          << dispatchCount << ", allocated=" << runtimeFrame.resourcesSets.size()
                          << ". Skipping remaining dispatches.\n";
                loggedResourceSetOverflow = true;
            }
            dispatchedAll = false;
            break;
        }
        const nrd::DispatchDesc &dispatch = dispatches[i];
        if (dispatch.pipelineIndex >= m_nrdPipelines.size()) {
            continue;
        }

        const nrd::PipelineDesc &pipelineDesc = instanceDesc->pipelines[dispatch.pipelineIndex];
        if (pipelineDesc.resourceRanges == nullptr || pipelineDesc.resourceRangesNum == 0) {
            continue;
        }

        uint32_t nextTexture = 0;
        uint32_t nextStorage = 0;
        uint32_t resourceIndex = 0;
        for (uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex) {
            const nrd::ResourceRangeDesc &rangeDesc = pipelineDesc.resourceRanges[rangeIndex];
            for (uint32_t j = 0; j < rangeDesc.descriptorsNum; ++j, ++resourceIndex) {
                if (resourceIndex >= dispatch.resourcesNum) {
                    break;
                }
                const nrd::ResourceDesc &resourceDesc = dispatch.resources[resourceIndex];

                vk::ImageView view = VK_NULL_HANDLE;
                if (resourceDesc.type == nrd::ResourceType::PERMANENT_POOL) {
                    if (resourceDesc.indexInPool < runtime.permanentPool.size()) {
                        view = *runtime.permanentPool[resourceDesc.indexInPool].image.view;
                    }
                } else if (resourceDesc.type == nrd::ResourceType::TRANSIENT_POOL) {
                    if (resourceDesc.indexInPool < runtime.transientPool.size()) {
                        view = *runtime.transientPool[resourceDesc.indexInPool].image.view;
                    }
                } else if (!getExternalView(resourceDesc.type, view)) {
                    continue;
                }

                if (resourceDesc.descriptorType == nrd::DescriptorType::TEXTURE) {
                    if (nextTexture < sampledInfos.size()) {
                        sampledInfos[nextTexture].imageView = view;
                        sampledInfos[nextTexture].imageLayout = vk::ImageLayout::eGeneral;
                        ++nextTexture;
                    }
                } else if (resourceDesc.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
                    if (nextStorage < storageInfos.size()) {
                        storageInfos[nextStorage].imageView = view;
                        storageInfos[nextStorage].imageLayout = vk::ImageLayout::eGeneral;
                        ++nextStorage;
                    }
                }
            }
        }

        std::vector<vk::WriteDescriptorSet> descriptorWrites;
        descriptorWrites.reserve(sampledInfos.size() + storageInfos.size());
        const vk::DescriptorSet resourceSetForDispatch = *runtimeFrame.resourcesSets[i];
        for (uint32_t slot = 0; slot < static_cast<uint32_t>(sampledInfos.size()); ++slot) {
            vk::WriteDescriptorSet sampledWrite{};
            sampledWrite.dstSet = resourceSetForDispatch;
            sampledWrite.dstBinding = m_nrdTextureBinding + slot;
            sampledWrite.descriptorType = vk::DescriptorType::eSampledImage;
            sampledWrite.descriptorCount = 1;
            sampledWrite.pImageInfo = &sampledInfos[slot];
            descriptorWrites.push_back(sampledWrite);
        }
        for (uint32_t slot = 0; slot < static_cast<uint32_t>(storageInfos.size()); ++slot) {
            vk::WriteDescriptorSet storageWrite{};
            storageWrite.dstSet = resourceSetForDispatch;
            storageWrite.dstBinding = m_nrdStorageBinding + slot;
            storageWrite.descriptorType = vk::DescriptorType::eStorageImage;
            storageWrite.descriptorCount = 1;
            storageWrite.pImageInfo = &storageInfos[slot];
            descriptorWrites.push_back(storageWrite);
        }
        m_context.getDevice().updateDescriptorSets(descriptorWrites, {});

        if (dispatch.constantBufferData != nullptr && dispatch.constantBufferDataSize > 0) {
            const uint32_t writeIndex = constantSetIndex % maxConstantsSets;
            const uint32_t maxSize = m_nrdConstantBufferSize;
            const uint32_t copySize = std::min(dispatch.constantBufferDataSize, maxSize);
            uint8_t *dst =
                static_cast<uint8_t *>(runtimeFrame.constantMapped) +
                static_cast<size_t>(writeIndex) * static_cast<size_t>(m_nrdConstantBufferStride);
            std::memcpy(dst, dispatch.constantBufferData, copySize);
            if (copySize < maxSize) {
                std::memset(dst + copySize, 0, maxSize - copySize);
            }
            ++constantSetIndex;
        }

        const uint32_t cbIndex =
            (constantSetIndex == 0) ? 0 : ((constantSetIndex - 1) % maxConstantsSets);
        const uint32_t dynamicOffset = cbIndex * m_nrdConstantBufferStride;
        const std::array<uint32_t, 1> dynamicOffsets = {dynamicOffset};

        commandBuffer.bindPipeline(
            vk::PipelineBindPoint::eCompute, *m_nrdPipelines[dispatch.pipelineIndex]
        );
        const vk::DescriptorSet resourcesSet = resourceSetForDispatch;
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            *m_nrdPipelineLayout,
            m_nrdSetResourcesIndex,
            resourcesSet,
            {}
        );
        const vk::DescriptorSet constantsSet = *runtimeFrame.constantsSet;
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            *m_nrdPipelineLayout,
            m_nrdSetConstantsIndex,
            constantsSet,
            dynamicOffsets
        );
        commandBuffer.dispatch(dispatch.gridWidth, dispatch.gridHeight, 1u);
    }
    setNrdValidForImage(dispatchedAll);
}
#endif

void VulkanRenderer::barrierNrdSignalsForCompute(
    vk::CommandBuffer commandBuffer, uint32_t imageIndex
) {
    if (m_nrdPerImage.empty()) {
        return;
    }

    const size_t resourceIndex = std::min<size_t>(imageIndex, m_nrdPerImage.size() - 1u);
    const NrdPerImageResources &nrd = m_nrdPerImage[resourceIndex];
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

auto makeInputBarrier = [&](vk::Image image) {
        vk::ImageMemoryBarrier barrier{};
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;

        barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead;

        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;

        return barrier;
    };

    auto makeHistoryBarrier = [&](vk::Image image) {
        vk::ImageMemoryBarrier barrier{};
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        return barrier;
    };

    std::array<vk::ImageMemoryBarrier, 5> barriers = {
        makeInputBarrier(*nrd.diffIn.image),
        makeInputBarrier(*nrd.normalRoughnessIn.image),
        makeInputBarrier(*nrd.motionIn.image),
        makeInputBarrier(*nrd.viewZIn.image),
        makeHistoryBarrier(*nrd.diffOut.image),
    };
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        {},
        {},
        barriers
    );
}

void VulkanRenderer::barrierNrdSignalsForComposite(
    vk::CommandBuffer commandBuffer, uint32_t imageIndex
) {
    if (m_nrdPerImage.empty()) {
        return;
    }

    const size_t resourceIndex = std::min<size_t>(imageIndex, m_nrdPerImage.size() - 1u);
    const NrdPerImageResources &nrd = m_nrdPerImage[resourceIndex];
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    auto makeShaderBarrier = [&](vk::Image image) {
        vk::ImageMemoryBarrier barrier{};
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        return barrier;
    };
    auto makeColorBarrier = [&](vk::Image image) {
        vk::ImageMemoryBarrier barrier{};
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        return barrier;
    };

    std::array<vk::ImageMemoryBarrier, 3> barriers = {
        makeShaderBarrier(*nrd.diffOut.image),
        makeColorBarrier(*nrd.composeBase.image),
        makeColorBarrier(*nrd.composeIndirect.image),
    };
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eFragmentShader,
        {},
        {},
        {},
        barriers
    );
}
