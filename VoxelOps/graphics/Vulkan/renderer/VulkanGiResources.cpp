#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
constexpr bool kRestirGiUseSpatialHistory = false;
constexpr uint32_t kRestirHistorySlot = 0u;

struct alignas(16) GiReservedStorageParamsGpu {
    glm::ivec4 originSpacingBlocks{0};
    glm::uvec4 reservedCounts{0u};
};

struct alignas(16) GiLightingParamsGpu {
    glm::uvec4 header{0u};
    glm::uvec4 pathConfig{1u, 0u, 0u, 0u};
    glm::uvec4 tracingConfig{0u, 0u, 0u, 0u};
    glm::vec4 tuning{1.0f, 0.55f, 0.70f, 0.00f};
    glm::vec4 sunDirection{0.25f, 0.85f, 0.42f, 0.0f};
    glm::ivec4 shadowOccupancyMinWordCount{0};
    glm::uvec4 shadowOccupancyDims{0u};
    glm::ivec4 shadowWorldBoundsXy{0};
    glm::ivec4 shadowWorldBoundsZ{0};
    glm::vec4 shadowParams{64.0f, 0.08f, 2.0f, 1.0f};
    glm::vec4 restirParams{0.00f, 0.00f, 0.0f, 0.0f};
    glm::vec4 denoiseParams{0.32f, 0.12f, 2.0f, 0.24f};
    glm::mat4 currViewProjection{1.0f};
    glm::mat4 prevViewProjection{1.0f};
    glm::mat4 nrdPrevViewProjection{1.0f};
    std::array<GiReservedStorageParamsGpu, 3> reservedStorage{};
};
} // namespace
template <typename T>
void VulkanRenderer::createPingPongImagePair(std::vector<std::array<T, 2>> &out, vk::Format format,
                                             vk::Extent2D extent, vk::ImageUsageFlags usage,
                                             const vk::ClearColorValue &clearValue,
                                             vk::raii::CommandBuffer &commandBuffer) {
    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();

    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    for (auto &pair : out) {
        for (auto &res : pair) {
            vk::ImageCreateInfo imageInfo{};
            imageInfo.imageType = vk::ImageType::e2D;
            imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1u);
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = format;
            imageInfo.tiling = vk::ImageTiling::eOptimal;
            imageInfo.initialLayout = vk::ImageLayout::eUndefined;
            imageInfo.usage = usage;
            imageInfo.samples = vk::SampleCountFlagBits::e1;
            imageInfo.sharingMode = vk::SharingMode::eExclusive;
            res.image = vk::raii::Image(device, imageInfo);

            const vk::MemoryRequirements requirements = res.image.getMemoryRequirements();
            vk::MemoryAllocateInfo allocInfo{};
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex =
                VulkanUtils::findMemoryType(physicalDevice, requirements.memoryTypeBits,
                                            vk::MemoryPropertyFlagBits::eDeviceLocal);
            res.memory = vk::raii::DeviceMemory(device, allocInfo);
            res.image.bindMemory(*res.memory, 0);

            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = *res.image;
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = range;
            viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            res.view = vk::raii::ImageView(device, viewInfo);

            vk::ImageMemoryBarrier toGeneral{};
            toGeneral.oldLayout = vk::ImageLayout::eUndefined;
            toGeneral.newLayout = vk::ImageLayout::eGeneral;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = *res.image;
            toGeneral.subresourceRange = range;
            toGeneral.srcAccessMask = {};
            toGeneral.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                      vk::AccessFlagBits::eShaderWrite |
                                      vk::AccessFlagBits::eTransferWrite;
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                          vk::PipelineStageFlagBits::eTransfer |
                                              vk::PipelineStageFlagBits::eFragmentShader |
                                              vk::PipelineStageFlagBits::eComputeShader,
                                          {}, {}, {}, toGeneral);
            commandBuffer.clearColorImage(*res.image, vk::ImageLayout::eGeneral, clearValue, range);
        }
    }
}

vk::raii::Sampler VulkanRenderer::createSharedRestirSampler() {
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    return vk::raii::Sampler(m_context.getDevice(), samplerInfo);
}

void VulkanRenderer::createRestirDiResources() {
    cleanupRestirDiResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const size_t imageCount = m_framebuffers.size();
    m_restirDiPerImage.resize(imageCount);
    m_restirDiWriteParityPerImage.assign(imageCount, 0u);
    m_restirDiValidPerImage.assign(imageCount, false);
    m_restirDiPrevViewProjectionPerImage.assign(imageCount, glm::mat4(1.0f));
    m_restirDiPrevViewPerImage.assign(imageCount, glm::mat4(1.0f));
    m_restirDiPrevProjectionPerImage.assign(imageCount, glm::mat4(1.0f));
    m_prevViewProjectionValidPerImage.assign(imageCount, false);

    m_restirDiSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(m_restirDiPerImage, vk::Format::eR16G16B16A16Sfloat, extent, kUsage,
                            vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}), cmd);
    VulkanUtils::endSingleTimeCommands(m_context.getDevice(), m_context.getGraphicsQueue(),
                                       std::move(cmd));
}

void VulkanRenderer::cleanupRestirDiResources() {
    m_restirDiSampler.clear();
    m_restirDiPerImage.clear();
    m_restirDiWriteParityPerImage.clear();
    m_restirDiValidPerImage.clear();
    m_restirDiPrevViewProjectionPerImage.clear();
    m_restirDiPrevViewPerImage.clear();
    m_restirDiPrevProjectionPerImage.clear();
    m_prevViewProjectionValidPerImage.clear();
}

void VulkanRenderer::createRestirValidationResources() {
    cleanupRestirValidationResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    m_restirValidationPerImage.resize(m_framebuffers.size());
    m_restirValidationSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(m_restirValidationPerImage, vk::Format::eR16G16B16A16Sfloat, extent,
                            kUsage,
                            vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.0f}), cmd);
    VulkanUtils::endSingleTimeCommands(m_context.getDevice(), m_context.getGraphicsQueue(),
                                       std::move(cmd));
}

void VulkanRenderer::cleanupRestirValidationResources() {
    m_restirValidationSampler.clear();
    m_restirValidationPerImage.clear();
}

void VulkanRenderer::createRestirMetaResources() {
    cleanupRestirMetaResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    m_restirMetaPerImage.resize(m_framebuffers.size());
    m_restirMetaSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(m_restirMetaPerImage, vk::Format::eR16G16B16A16Sfloat, extent, kUsage,
                            vk::ClearColorValue(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}), cmd);
    VulkanUtils::endSingleTimeCommands(m_context.getDevice(), m_context.getGraphicsQueue(),
                                       std::move(cmd));
}

void VulkanRenderer::cleanupRestirMetaResources() {
    m_restirMetaSampler.clear();
    m_restirMetaPerImage.clear();
}

void VulkanRenderer::createRestirGiResources() {
    cleanupRestirGiResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const size_t imageCount = m_framebuffers.size();
    m_restirGiPerImage.resize(imageCount);
    m_restirGiMetaPerImage.resize(imageCount);
    m_restirGiSampler = createSharedRestirSampler();
    m_restirGiMetaSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    // Both pairs go into one command buffer — single queue submission
    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(m_restirGiPerImage, vk::Format::eR16G16B16A16Sfloat, extent, kUsage,
                            vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}), cmd);
    createPingPongImagePair(m_restirGiMetaPerImage, vk::Format::eR16G16B16A16Sfloat, extent, kUsage,
                            vk::ClearColorValue(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}), cmd);
    VulkanUtils::endSingleTimeCommands(m_context.getDevice(), m_context.getGraphicsQueue(),
                                       std::move(cmd));
}

void VulkanRenderer::cleanupRestirGiResources() {
    m_restirGiSampler.clear();
    m_restirGiMetaSampler.clear();
    m_restirGiPerImage.clear();
    m_restirGiMetaPerImage.clear();
}

void VulkanRenderer::createGiDescriptorResources() {
    cleanupGiDescriptorResources();
    if (m_framebuffers.empty()) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();

    m_giRtDescriptorEnabled = m_context.isHardwareRayTracingSupported();

    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.reserve(m_giRtDescriptorEnabled ? GI_BINDING_COUNT : GI_RT_SCENE_BINDING);
    auto addBinding = [&bindings](uint32_t binding, vk::DescriptorType type) {
        vk::DescriptorSetLayoutBinding layoutBinding{};
        layoutBinding.binding = binding;
        layoutBinding.descriptorType = type;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
        bindings.push_back(layoutBinding);
    };
    for (uint32_t i = 0; i < GI_RESERVED_STORAGE_BINDINGS; ++i) {
        addBinding(i, vk::DescriptorType::eStorageBuffer);
    }
    addBinding(GI_PARAM_BINDING, vk::DescriptorType::eUniformBuffer);
    addBinding(GI_SHADOW_OCCUPANCY_BINDING, vk::DescriptorType::eStorageBuffer);
    addBinding(GI_MATERIAL_BINDING, vk::DescriptorType::eStorageBuffer);
    addBinding(GI_RESTIR_DI_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_RESTIR_DI_CURR_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_RESTIR_VALIDATION_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_RESTIR_VALIDATION_CURR_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_RESTIR_META_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_RESTIR_META_CURR_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_RESTIR_GI_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_RESTIR_GI_CURR_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_RESTIR_GI_META_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_RESTIR_GI_META_CURR_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_DIFF_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_NORMAL_ROUGHNESS_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_MV_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_VIEWZ_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_DIFF_OUT_BINDING, vk::DescriptorType::eCombinedImageSampler);
    if (m_giRtDescriptorEnabled) {
        addBinding(GI_RT_SCENE_BINDING, vk::DescriptorType::eAccelerationStructureKHR);
    }

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    m_giDescriptorSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);

    std::vector<vk::DescriptorPoolSize> poolSizes;
    poolSizes.reserve(m_giRtDescriptorEnabled ? 5 : 4);
    vk::DescriptorPoolSize storagePool{};
    storagePool.type = vk::DescriptorType::eStorageBuffer;
    storagePool.descriptorCount =
        static_cast<uint32_t>(m_framebuffers.size()) * (GI_RESERVED_STORAGE_BINDINGS + 2);
    poolSizes.push_back(storagePool);
    vk::DescriptorPoolSize uniformPool{};
    uniformPool.type = vk::DescriptorType::eUniformBuffer;
    uniformPool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size());
    poolSizes.push_back(uniformPool);
    vk::DescriptorPoolSize sampledPool{};
    sampledPool.type = vk::DescriptorType::eCombinedImageSampler;
    sampledPool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size()) * 6u;
    poolSizes.push_back(sampledPool);
    vk::DescriptorPoolSize storageImagePool{};
    storageImagePool.type = vk::DescriptorType::eStorageImage;
    storageImagePool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size()) * 9u;
    poolSizes.push_back(storageImagePool);
    if (m_giRtDescriptorEnabled) {
        vk::DescriptorPoolSize accelPool{};
        accelPool.type = vk::DescriptorType::eAccelerationStructureKHR;
        accelPool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size());
        poolSizes.push_back(accelPool);
    }

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = static_cast<uint32_t>(m_framebuffers.size());
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    m_giDescriptorPool = vk::raii::DescriptorPool(device, poolInfo);

    std::vector<vk::DescriptorSetLayout> layouts(m_framebuffers.size(), *m_giDescriptorSetLayout);
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *m_giDescriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    m_giDescriptorSets = device.allocateDescriptorSets(allocateInfo);

    m_giFallbackReservedStorageBuffers.clear();
    m_giFallbackReservedStorageBufferMemory.clear();
    m_giFallbackReservedStorageBuffers.reserve(GI_RESERVED_STORAGE_BINDINGS);
    m_giFallbackReservedStorageBufferMemory.reserve(GI_RESERVED_STORAGE_BINDINGS);
    for (uint32_t i = 0; i < GI_RESERVED_STORAGE_BINDINGS; ++i) {
        m_giFallbackReservedStorageBuffers.emplace_back(nullptr);
        m_giFallbackReservedStorageBufferMemory.emplace_back(nullptr);
        VulkanUtils::createBuffer(
            device, physicalDevice, sizeof(uint32_t),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            m_giFallbackReservedStorageBuffers.back(),
            m_giFallbackReservedStorageBufferMemory.back());

        const uint32_t zeroWord = 0u;
        void *mapped = m_giFallbackReservedStorageBufferMemory.back().mapMemory(0, VK_WHOLE_SIZE);
        std::memcpy(mapped, &zeroWord, sizeof(zeroWord));
        m_giFallbackReservedStorageBufferMemory.back().unmapMemory();
    }

    VulkanUtils::createBuffer(
        device, physicalDevice, sizeof(uint32_t), vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        m_giFallbackShadowOccupancyBuffer, m_giFallbackShadowOccupancyBufferMemory);
    {
        const uint32_t zeroWord = 0u;
        void *mapped = m_giFallbackShadowOccupancyBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
        std::memcpy(mapped, &zeroWord, sizeof(zeroWord));
        m_giFallbackShadowOccupancyBufferMemory.unmapMemory();
    }

    VulkanUtils::createBuffer(
        device, physicalDevice, sizeof(uint32_t), vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        m_giFallbackMaterialBuffer, m_giFallbackMaterialBufferMemory);
    {
        const uint32_t fallbackMaterial = 0u;
        void *mapped = m_giFallbackMaterialBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
        std::memcpy(mapped, &fallbackMaterial, sizeof(fallbackMaterial));
        m_giFallbackMaterialBufferMemory.unmapMemory();
    }

    for (PerImageDrawResources &resources : m_perImageDrawResources) {
        VulkanUtils::createBuffer(device, physicalDevice, sizeof(GiLightingParamsGpu),
                                  vk::BufferUsageFlagBits::eUniformBuffer,
                                  vk::MemoryPropertyFlagBits::eHostVisible |
                                      vk::MemoryPropertyFlagBits::eHostCoherent,
                                  resources.giParamsBuffer, resources.giParamsBufferMemory);
        resources.giParamsMapped = resources.giParamsBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
    }
}

void VulkanRenderer::updateGiDescriptorSet(uint32_t imageIndex, const FrameRenderData &frameData,
                                           const glm::mat4 &viewProjection) {
    if (imageIndex >= m_perImageDrawResources.size() || imageIndex >= m_giDescriptorSets.size()) {
        return;
    }

    PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
    if (resources.giParamsBuffer == nullptr || resources.giParamsMapped == nullptr) {
        return;
    }

    const uint32_t historyIndex = kRestirHistorySlot;
    GiLightingParamsGpu params{};
    params.header.x = 0u;
    params.header.y = frameData.giLighting.sunShadowsEnabled ? 1u : 0u;
    params.header.z = frameData.giLighting.pathTracingEnabled ? 1u : 0u;
    params.header.w = frameData.giLighting.nrdDebugView;
    params.pathConfig.x = std::max(1u, frameData.giLighting.pathTraceRaysPerPixel);
    const bool hasPrevViewProjection = frameData.giLighting.enabled &&
                                       !frameData.giLighting.resetHistory &&
                                       historyIndex < m_restirDiPrevViewProjectionPerImage.size() &&
                                       historyIndex < m_prevViewProjectionValidPerImage.size() &&
                                       m_prevViewProjectionValidPerImage[historyIndex];
    const bool hasNrdPrevViewProjection = frameData.giLighting.enabled &&
                                          !frameData.giLighting.resetHistory &&
                                          m_nrdPrevMatricesValid;
    const bool restirHistoryValid = frameData.giLighting.pathTracingEnabled &&
                                    !frameData.giLighting.resetHistory && hasPrevViewProjection;
    params.pathConfig.y = restirHistoryValid ? 1u : 0u;
    params.pathConfig.z = m_frameCounterLow;
    params.pathConfig.w = frameData.giLighting.resetHistory ? 1u : 0u;
    params.tracingConfig.x =
        (frameData.giLighting.tracingBackend == GiTracingBackend::HardwareRt) ? 1u : 0u;
    params.tracingConfig.y = frameData.giLighting.hardwareRayTracingSupported ? 1u : 0u;
    params.tracingConfig.z =
        (m_giRtDescriptorEnabled && frameData.giLighting.sceneTlas != VK_NULL_HANDLE) ? 1u : 0u;
    const bool nrdHistoryValid = frameData.giLighting.pathTracingEnabled &&
                                 !frameData.giLighting.resetHistory && hasNrdPrevViewProjection &&
                                 historyIndex < m_nrdValidPerImage.size() &&
                                 m_nrdValidPerImage[historyIndex];
    params.tracingConfig.w = nrdHistoryValid ? 1u : 0u;
    params.tuning.x = frameData.giLighting.baseDiffuse;
    params.tuning.y = frameData.giLighting.giIntensity;
    params.tuning.z = frameData.giLighting.sunIntensity;
    params.tuning.w = frameData.giLighting.sunShadowMinVisibility;
    params.sunDirection = glm::vec4(frameData.giLighting.sunDirection, 0.0f);
    params.shadowOccupancyMinWordCount =
        glm::ivec4(frameData.giLighting.shadowOccupancyMinBlocks,
                   static_cast<int32_t>(frameData.giLighting.shadowOccupancyWordCount));
    params.shadowOccupancyDims = glm::uvec4(frameData.giLighting.shadowOccupancyDims, 0u);
    params.shadowWorldBoundsXy = frameData.giLighting.shadowWorldBoundsXy;
    params.shadowWorldBoundsZ = frameData.giLighting.shadowWorldBoundsZ;
    params.shadowParams.x = frameData.giLighting.sunShadowMaxDistance;
    params.shadowParams.y = 0.08f;
    params.shadowParams.z =
        static_cast<float>(std::max(1u, frameData.giLighting.pathTraceMaxBounces));
    params.shadowParams.w = std::max(0.0f, frameData.giLighting.pathTraceSkyIntensity);
    params.restirParams.x = glm::clamp(frameData.giLighting.restirTemporalBlend, 0.0f, 1.0f);
    params.restirParams.y = glm::clamp(frameData.giLighting.restirSpatialReuse, 0.0f, 1.0f);
    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width > 0 && extent.height > 0) {
        params.restirParams.z = 1.0f / static_cast<float>(extent.width);
        params.restirParams.w = 1.0f / static_cast<float>(extent.height);
    }
    params.denoiseParams.x = glm::clamp(frameData.giLighting.denoiseTemporalBlend, 0.0f, 1.0f);
    params.denoiseParams.y = glm::clamp(frameData.giLighting.denoiseSpatialWeight, 0.0f, 1.0f);
    params.denoiseParams.z = std::max(frameData.giLighting.denoiseLumaPhi, 0.01f);
    params.denoiseParams.w = glm::clamp(frameData.giLighting.denoiseMomentBlend, 0.0f, 1.0f);
    params.currViewProjection = viewProjection;
    params.prevViewProjection =
        hasPrevViewProjection ? m_restirDiPrevViewProjectionPerImage[historyIndex] : viewProjection;
    params.nrdPrevViewProjection =
        hasNrdPrevViewProjection ? m_nrdPrevViewProjection : viewProjection;

    std::memcpy(resources.giParamsMapped, &params, sizeof(params));

    std::array<vk::DescriptorBufferInfo, GI_RESERVED_STORAGE_BINDINGS + 3> bufferInfos{};
    for (uint32_t i = 0; i < GI_RESERVED_STORAGE_BINDINGS; ++i) {
        VkBuffer source = VK_NULL_HANDLE;
        if (i < m_giFallbackReservedStorageBuffers.size() &&
            m_giFallbackReservedStorageBuffers[i] != nullptr) {
            source = *m_giFallbackReservedStorageBuffers[i];
        }
        bufferInfos[i].buffer = source;
        bufferInfos[i].offset = 0;
        bufferInfos[i].range = VK_WHOLE_SIZE;
    }

    const bool needsTraceOccupancy =
        frameData.giLighting.sunShadowsEnabled || frameData.giLighting.pathTracingEnabled;
    VkBuffer shadowSource =
        needsTraceOccupancy ? frameData.giLighting.shadowOccupancyBuffer : VK_NULL_HANDLE;
    if (shadowSource == VK_NULL_HANDLE && m_giFallbackShadowOccupancyBuffer != nullptr) {
        shadowSource = *m_giFallbackShadowOccupancyBuffer;
    }
    bufferInfos[GI_SHADOW_OCCUPANCY_BINDING].buffer = shadowSource;
    bufferInfos[GI_SHADOW_OCCUPANCY_BINDING].offset = 0;
    bufferInfos[GI_SHADOW_OCCUPANCY_BINDING].range = VK_WHOLE_SIZE;

    VkBuffer materialSource = frameData.giLighting.traceMaterialBuffer;
    if (materialSource == VK_NULL_HANDLE && m_giFallbackMaterialBuffer != nullptr) {
        materialSource = *m_giFallbackMaterialBuffer;
    }
    bufferInfos[GI_MATERIAL_BINDING].buffer = materialSource;
    bufferInfos[GI_MATERIAL_BINDING].offset = 0;
    bufferInfos[GI_MATERIAL_BINDING].range = VK_WHOLE_SIZE;

    bufferInfos[GI_PARAM_BINDING].buffer = *resources.giParamsBuffer;
    bufferInfos[GI_PARAM_BINDING].offset = 0;
    bufferInfos[GI_PARAM_BINDING].range = sizeof(GiLightingParamsGpu);

    vk::DescriptorImageInfo prevRestirInfo{};
    vk::DescriptorImageInfo currRestirInfo{};
    vk::DescriptorImageInfo prevValidationInfo{};
    vk::DescriptorImageInfo currValidationInfo{};
    vk::DescriptorImageInfo prevMetaInfo{};
    vk::DescriptorImageInfo currMetaInfo{};
    vk::DescriptorImageInfo prevRestirGiInfo{};
    vk::DescriptorImageInfo currRestirGiInfo{};
    vk::DescriptorImageInfo prevRestirGiMetaInfo{};
    vk::DescriptorImageInfo currRestirGiMetaInfo{};
    vk::DescriptorImageInfo nrdDiffInInfo{};
    vk::DescriptorImageInfo nrdNormalRoughnessInInfo{};
    vk::DescriptorImageInfo nrdMvInInfo{};
    vk::DescriptorImageInfo nrdViewZInInfo{};
    vk::DescriptorImageInfo nrdDiffOutInfo{};
    const bool restirReady =
        historyIndex < m_restirDiPerImage.size() &&
        historyIndex < m_restirValidationPerImage.size() &&
        historyIndex < m_restirMetaPerImage.size() && historyIndex < m_restirGiPerImage.size() &&
        historyIndex < m_restirGiMetaPerImage.size() &&
        historyIndex < m_restirGiSpatialPerImage.size() &&
        historyIndex < m_restirGiSpatialMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size() && m_restirDiSampler != nullptr &&
        m_restirValidationSampler != nullptr && m_restirMetaSampler != nullptr &&
        m_restirGiSampler != nullptr && m_restirGiMetaSampler != nullptr;
    if (restirReady) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirDiPerImage[historyIndex];
        prevRestirInfo.sampler = *m_restirDiSampler;
        prevRestirInfo.imageView = *perImage[readParity].view;
        prevRestirInfo.imageLayout = vk::ImageLayout::eGeneral;
        currRestirInfo.sampler = VK_NULL_HANDLE;
        currRestirInfo.imageView = *perImage[writeParity].view;
        currRestirInfo.imageLayout = vk::ImageLayout::eGeneral;

        const auto &validationPerImage = m_restirValidationPerImage[historyIndex];
        prevValidationInfo.sampler = *m_restirValidationSampler;
        prevValidationInfo.imageView = *validationPerImage[readParity].view;
        prevValidationInfo.imageLayout = vk::ImageLayout::eGeneral;
        currValidationInfo.sampler = VK_NULL_HANDLE;
        currValidationInfo.imageView = *validationPerImage[writeParity].view;
        currValidationInfo.imageLayout = vk::ImageLayout::eGeneral;

        const auto &metaPerImage = m_restirMetaPerImage[historyIndex];
        prevMetaInfo.sampler = *m_restirMetaSampler;
        prevMetaInfo.imageView = *metaPerImage[readParity].view;
        prevMetaInfo.imageLayout = vk::ImageLayout::eGeneral;
        currMetaInfo.sampler = VK_NULL_HANDLE;
        currMetaInfo.imageView = *metaPerImage[writeParity].view;
        currMetaInfo.imageLayout = vk::ImageLayout::eGeneral;

        const auto &giPerImage = m_restirGiPerImage[historyIndex];
        const auto &giSpatialPerImage = m_restirGiSpatialPerImage[historyIndex];
        const auto &giHistoryPerImage = kRestirGiUseSpatialHistory ? giSpatialPerImage : giPerImage;
        prevRestirGiInfo.sampler = *m_restirGiSampler;
        prevRestirGiInfo.imageView = *giHistoryPerImage[readParity].view;
        prevRestirGiInfo.imageLayout = vk::ImageLayout::eGeneral;
        currRestirGiInfo.sampler = VK_NULL_HANDLE;
        currRestirGiInfo.imageView = *giPerImage[writeParity].view;
        currRestirGiInfo.imageLayout = vk::ImageLayout::eGeneral;

        const auto &giMetaPerImage = m_restirGiMetaPerImage[historyIndex];
        const auto &giSpatialMetaPerImage = m_restirGiSpatialMetaPerImage[historyIndex];
        const auto &giHistoryMetaPerImage =
            kRestirGiUseSpatialHistory ? giSpatialMetaPerImage : giMetaPerImage;
        prevRestirGiMetaInfo.sampler = *m_restirGiMetaSampler;
        prevRestirGiMetaInfo.imageView = *giHistoryMetaPerImage[readParity].view;
        prevRestirGiMetaInfo.imageLayout = vk::ImageLayout::eGeneral;
        currRestirGiMetaInfo.sampler = VK_NULL_HANDLE;
        currRestirGiMetaInfo.imageView = *giMetaPerImage[writeParity].view;
        currRestirGiMetaInfo.imageLayout = vk::ImageLayout::eGeneral;
    }

    const NrdPerImageResources *nrdResources = nullptr;
    if (!m_nrdPerImage.empty()) {
        nrdResources = &m_nrdPerImage[0];
    } else if (m_nrdFallbackReady) {
        nrdResources = &m_nrdFallback;
    }
    if (nrdResources != nullptr) {
        nrdDiffInInfo.sampler = VK_NULL_HANDLE;
        nrdDiffInInfo.imageView = *nrdResources->diffIn.view;
        nrdDiffInInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdNormalRoughnessInInfo.sampler = VK_NULL_HANDLE;
        nrdNormalRoughnessInInfo.imageView = *nrdResources->normalRoughnessIn.view;
        nrdNormalRoughnessInInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdMvInInfo.sampler = VK_NULL_HANDLE;
        nrdMvInInfo.imageView = *nrdResources->motionIn.view;
        nrdMvInInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdViewZInInfo.sampler = VK_NULL_HANDLE;
        nrdViewZInInfo.imageView = *nrdResources->viewZIn.view;
        nrdViewZInInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdDiffOutInfo.sampler =
            (m_nrdOutputSampler != nullptr) ? *m_nrdOutputSampler : VK_NULL_HANDLE;
        nrdDiffOutInfo.imageView = *nrdResources->diffOut.view;
        nrdDiffOutInfo.imageLayout = vk::ImageLayout::eGeneral;
    } else if (!m_loggedMissingNrdResources) {
        std::cerr << "[Vulkan][NRD] Missing signal resources, GI descriptors are using null NRD "
                     "bindings.\n";
        m_loggedMissingNrdResources = true;
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(m_giRtDescriptorEnabled ? GI_BINDING_COUNT : GI_RT_SCENE_BINDING);
    auto pushBufferWrite = [&](uint32_t binding, vk::DescriptorType type,
                               const vk::DescriptorBufferInfo *info) {
        vk::WriteDescriptorSet write{};
        write.dstSet = *m_giDescriptorSets[imageIndex];
        write.dstBinding = binding;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pBufferInfo = info;
        writes.push_back(write);
    };
    auto pushImageWrite = [&](uint32_t binding, vk::DescriptorType type,
                              const vk::DescriptorImageInfo *info) {
        vk::WriteDescriptorSet write{};
        write.dstSet = *m_giDescriptorSets[imageIndex];
        write.dstBinding = binding;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pImageInfo = info;
        writes.push_back(write);
    };

    for (uint32_t i = 0; i < GI_RESERVED_STORAGE_BINDINGS; ++i) {
        pushBufferWrite(i, vk::DescriptorType::eStorageBuffer, &bufferInfos[i]);
    }
    pushBufferWrite(GI_PARAM_BINDING, vk::DescriptorType::eUniformBuffer,
                    &bufferInfos[GI_PARAM_BINDING]);
    pushBufferWrite(GI_SHADOW_OCCUPANCY_BINDING, vk::DescriptorType::eStorageBuffer,
                    &bufferInfos[GI_SHADOW_OCCUPANCY_BINDING]);
    pushBufferWrite(GI_MATERIAL_BINDING, vk::DescriptorType::eStorageBuffer,
                    &bufferInfos[GI_MATERIAL_BINDING]);
    pushImageWrite(GI_RESTIR_DI_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler,
                   &prevRestirInfo);
    pushImageWrite(GI_RESTIR_DI_CURR_BINDING, vk::DescriptorType::eStorageImage, &currRestirInfo);
    pushImageWrite(GI_RESTIR_VALIDATION_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler,
                   &prevValidationInfo);
    pushImageWrite(GI_RESTIR_VALIDATION_CURR_BINDING, vk::DescriptorType::eStorageImage,
                   &currValidationInfo);
    pushImageWrite(GI_RESTIR_META_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler,
                   &prevMetaInfo);
    pushImageWrite(GI_RESTIR_META_CURR_BINDING, vk::DescriptorType::eStorageImage, &currMetaInfo);
    pushImageWrite(GI_RESTIR_GI_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler,
                   &prevRestirGiInfo);
    pushImageWrite(GI_RESTIR_GI_CURR_BINDING, vk::DescriptorType::eStorageImage, &currRestirGiInfo);
    pushImageWrite(GI_RESTIR_GI_META_PREV_BINDING, vk::DescriptorType::eCombinedImageSampler,
                   &prevRestirGiMetaInfo);
    pushImageWrite(GI_RESTIR_GI_META_CURR_BINDING, vk::DescriptorType::eStorageImage,
                   &currRestirGiMetaInfo);
    pushImageWrite(GI_NRD_DIFF_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdDiffInInfo);
    pushImageWrite(GI_NRD_NORMAL_ROUGHNESS_IN_BINDING, vk::DescriptorType::eStorageImage,
                   &nrdNormalRoughnessInInfo);
    pushImageWrite(GI_NRD_MV_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdMvInInfo);
    pushImageWrite(GI_NRD_VIEWZ_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdViewZInInfo);
    pushImageWrite(GI_NRD_DIFF_OUT_BINDING, vk::DescriptorType::eCombinedImageSampler,
                   &nrdDiffOutInfo);

    vk::WriteDescriptorSetAccelerationStructureKHR rtSceneInfo{};
    vk::AccelerationStructureKHR rtSceneHandle = frameData.giLighting.sceneTlas;
    if (m_giRtDescriptorEnabled) {
        rtSceneInfo.accelerationStructureCount = 1;
        rtSceneInfo.pAccelerationStructures = &rtSceneHandle;

        vk::WriteDescriptorSet rtSceneWrite{};
        rtSceneWrite.pNext = &rtSceneInfo;
        rtSceneWrite.dstSet = *m_giDescriptorSets[imageIndex];
        rtSceneWrite.dstBinding = GI_RT_SCENE_BINDING;
        rtSceneWrite.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
        rtSceneWrite.descriptorCount = 1;
        writes.push_back(rtSceneWrite);
    }

    const vk::raii::Device &device = m_context.getDevice();
    device.updateDescriptorSets(writes, {});
}

void VulkanRenderer::cleanupGiDescriptorResources() {
    m_giDescriptorSets.clear();
    m_giDescriptorPool.clear();
    m_giDescriptorSetLayout.clear();
    m_giRtDescriptorEnabled = false;
    m_giFallbackReservedStorageBuffers.clear();
    m_giFallbackReservedStorageBufferMemory.clear();
    m_giFallbackShadowOccupancyBuffer.clear();
    m_giFallbackShadowOccupancyBufferMemory.clear();
    m_giFallbackMaterialBuffer.clear();
    m_giFallbackMaterialBufferMemory.clear();
}

