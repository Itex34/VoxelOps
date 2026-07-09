#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
    // GPU transport layout only.
    // Runtime values are assigned each frame in updateGiDescriptorSet(...) from frameData.giLighting.
    struct alignas(16) GiLightingParamsGpu {
        glm::uvec4 header{0u};
        glm::uvec4 pathConfig{1u, 0u, 0u, 0u}; // x=rays, y=checkerboard, z=frame, w=history reset
        glm::uvec4 tracingConfig{0u, 0u, 0u, 0u};
        glm::uvec4 nrdEncoding{0u}; // {normalEncoding, roughnessEncoding}
        glm::vec4 tuning{0.0f}; // x = base diffuse, y = gi intensity, z = sun intensity, w = sun shadow min visibility
        glm::vec4 sunDirection{0.0f};
        glm::ivec4 shadowOccupancyMinWordCount{0};
        glm::uvec4 shadowOccupancyDims{0u};
        glm::ivec4 shadowWorldBoundsXy{0};
        glm::ivec4 shadowWorldBoundsZ{0};
        glm::vec4 shadowParams{0.0f};
        glm::vec4 screenParams{1.0f, 1.0f, 0.0f, 0.0f}; // x=RT render scale, y=RT ray density, zw=inv viewport
        glm::vec4 denoiseParams{0.0f};
        glm::vec4 nrdHitDistanceParams{0.0f};
        glm::mat4 currViewProjection{1.0f};
        glm::mat4 prevViewProjection{1.0f};
        glm::mat4 nrdPrevViewProjection{1.0f};
    };
} // namespace

void VulkanRenderer::createGiDescriptorResources() {
    cleanupGiDescriptorResources();
    if (m_framebuffers.empty()) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    const VmaAllocator allocator = m_context.getVmaAllocator();

    m_giRtDescriptorEnabled = m_context.isHardwareRayTracingSupported();

    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.reserve(m_giRtDescriptorEnabled ? GI_BINDING_COUNT : (GI_BINDING_COUNT - 1u));
    auto addBinding = [&bindings](uint32_t binding, vk::DescriptorType type) {
        vk::DescriptorSetLayoutBinding layoutBinding{};
        layoutBinding.binding = binding;
        layoutBinding.descriptorType = type;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
        bindings.push_back(layoutBinding);
    };
    addBinding(GI_PARAM_BINDING, vk::DescriptorType::eUniformBuffer);
    addBinding(GI_SHADOW_OCCUPANCY_BINDING, vk::DescriptorType::eStorageBuffer);
    addBinding(GI_MATERIAL_BINDING, vk::DescriptorType::eStorageBuffer);
    addBinding(GI_NRD_DIFF_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_NORMAL_ROUGHNESS_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_MV_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_VIEWZ_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_DIFF_OUT_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_NRD_COMPOSE_BASE_STORAGE_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_COMPOSE_INDIRECT_STORAGE_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_COMPOSE_BASE_SAMPLED_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(
        GI_NRD_COMPOSE_INDIRECT_SAMPLED_BINDING, vk::DescriptorType::eCombinedImageSampler
    );
    addBinding(GI_NRD_SHADOW_IN_BINDING, vk::DescriptorType::eStorageImage);
    addBinding(GI_NRD_SHADOW_OUT_BINDING, vk::DescriptorType::eCombinedImageSampler);
    addBinding(GI_BLUE_NOISE_BINDING, vk::DescriptorType::eCombinedImageSampler);
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
    storagePool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size()) * 2u;
    poolSizes.push_back(storagePool);
    vk::DescriptorPoolSize uniformPool{};
    uniformPool.type = vk::DescriptorType::eUniformBuffer;
    uniformPool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size());
    poolSizes.push_back(uniformPool);
    vk::DescriptorPoolSize sampledPool{};
    sampledPool.type = vk::DescriptorType::eCombinedImageSampler;
    sampledPool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size()) * 5u;
    poolSizes.push_back(sampledPool);
    vk::DescriptorPoolSize storageImagePool{};
    storageImagePool.type = vk::DescriptorType::eStorageImage;
    storageImagePool.descriptorCount = static_cast<uint32_t>(m_framebuffers.size()) * 7u;
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

    VulkanUtils::createBuffer(
        allocator,
        sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        m_giFallbackShadowOccupancyBuffer
    );
    {
        const uint32_t zeroWord = 0u;
        void *mapped = VulkanUtils::mapAllocation(m_giFallbackShadowOccupancyBuffer);
        std::memcpy(mapped, &zeroWord, sizeof(zeroWord));
        VulkanUtils::unmapAllocation(m_giFallbackShadowOccupancyBuffer);
    }

    VulkanUtils::createBuffer(
        allocator,
        sizeof(uint32_t),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        m_giFallbackMaterialBuffer
    );
    {
        const uint32_t fallbackMaterial = 0u;
        void *mapped = VulkanUtils::mapAllocation(m_giFallbackMaterialBuffer);
        std::memcpy(mapped, &fallbackMaterial, sizeof(fallbackMaterial));
        VulkanUtils::unmapAllocation(m_giFallbackMaterialBuffer);
    }

    for (PerImageDrawResources &resources : m_perImageDrawResources) {
        VulkanUtils::createBuffer(
            allocator,
            sizeof(GiLightingParamsGpu),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            resources.giParamsBuffer
        );
        resources.giParamsMapped = VulkanUtils::mapAllocation(resources.giParamsBuffer);
    }
}

void VulkanRenderer::updateGiDescriptorSet(
    uint32_t imageIndex, const FrameRenderData &frameData, const glm::mat4 &viewProjection
) {
    if (imageIndex >= m_perImageDrawResources.size() || imageIndex >= m_giDescriptorSets.size()) {
        return;
    }

    PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
    if (resources.giParamsBuffer == nullptr || resources.giParamsMapped == nullptr) {
        return;
    }

    GiLightingParamsGpu params{};
    params.header.x = frameData.giLighting.enabled ? 1u : 0u;
    params.header.y = frameData.giLighting.sunShadowsEnabled ? 1u : 0u;
    params.header.z = frameData.giLighting.pathTracingEnabled ? 1u : 0u;
    params.header.w = frameData.giLighting.nrdDebugView;
    params.pathConfig.x = std::max(1u, frameData.giLighting.pathTraceRaysPerPixel);
    const bool hasNrdPrevViewProjection = frameData.giLighting.enabled &&
                                          !frameData.giLighting.resetHistory &&
                                          m_nrdPrevMatricesValid;
    const float rtRenderScale =
        glm::clamp(frameData.giLighting.pathTraceRenderScale, 0.25f, 1.0f);
    params.pathConfig.y =
        (frameData.giLighting.pathTraceCheckerboard != 0u || rtRenderScale < 0.999f) ? 1u : 0u;
    params.pathConfig.z = m_frameCounterLow;
    params.pathConfig.w = frameData.giLighting.resetHistory ? 1u : 0u;
    const uint32_t historyIndex =
        m_nrdValidPerImage.empty()
            ? 0u
            : static_cast<uint32_t>(std::min<size_t>(imageIndex, m_nrdValidPerImage.size() - 1u));
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
    params.nrdEncoding.x = (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->normalEncoding() : 2u;
    params.nrdEncoding.y = (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->roughnessEncoding() : 1u;
    params.nrdEncoding.z = frameData.giLighting.nrdGuideOverride;
    params.nrdEncoding.w = 0u;
    params.tuning.x = frameData.giLighting.baseDiffuse;
    params.tuning.y = frameData.giLighting.giIntensity;
    params.tuning.z = frameData.giLighting.sunIntensity;
    params.tuning.w = frameData.giLighting.sunShadowMinVisibility;
    params.sunDirection = glm::vec4(frameData.giLighting.sunDirection, 0.0f);
    params.shadowOccupancyMinWordCount = glm::ivec4(
        frameData.giLighting.shadowOccupancyMinBlocks,
        static_cast<int32_t>(frameData.giLighting.shadowOccupancyWordCount)
    );
    params.shadowOccupancyDims = glm::uvec4(frameData.giLighting.shadowOccupancyDims, 0u);
    params.shadowWorldBoundsXy = frameData.giLighting.shadowWorldBoundsXy;
    params.shadowWorldBoundsZ = frameData.giLighting.shadowWorldBoundsZ;
    params.shadowParams.x = frameData.giLighting.sunShadowMaxDistance;
    params.shadowParams.y = 0.08f;
    params.shadowParams.z =
        static_cast<float>(std::max(1u, frameData.giLighting.pathTraceMaxBounces));
    params.shadowParams.w = std::max(0.0f, frameData.giLighting.pathTraceSkyIntensity);
    const vk::Extent2D extent = m_context.getSwapchainExtent();
    params.screenParams.x = rtRenderScale;
    params.screenParams.y = glm::clamp(rtRenderScale * rtRenderScale, 0.0f, 1.0f);
    if (extent.width > 0 && extent.height > 0) {
        params.screenParams.z = 1.0f / static_cast<float>(extent.width);
        params.screenParams.w = 1.0f / static_cast<float>(extent.height);
    }
    params.denoiseParams.x = glm::clamp(frameData.giLighting.denoiseTemporalBlend, 0.0f, 1.0f);
    params.denoiseParams.y = glm::clamp(frameData.giLighting.denoiseSpatialWeight, 0.0f, 1.0f);
    params.denoiseParams.z = std::max(frameData.giLighting.denoiseLumaPhi, 0.01f);
    params.denoiseParams.w = glm::clamp(frameData.giLighting.denoiseMomentBlend, 0.0f, 1.0f);
    params.nrdHitDistanceParams = glm::vec4(frameData.giLighting.nrdHitDistanceParams, 0.0f);
    params.currViewProjection = viewProjection;
    params.prevViewProjection = viewProjection;
    params.nrdPrevViewProjection =
        hasNrdPrevViewProjection ? m_nrdPrevViewProjection : viewProjection;

    std::memcpy(resources.giParamsMapped, &params, sizeof(params));

    std::array<vk::DescriptorBufferInfo, 3> bufferInfos{};

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

    vk::DescriptorImageInfo nrdDiffInInfo{};
    vk::DescriptorImageInfo nrdNormalRoughnessInInfo{};
    vk::DescriptorImageInfo nrdMvInInfo{};
    vk::DescriptorImageInfo nrdViewZInInfo{};
    vk::DescriptorImageInfo nrdDiffOutInfo{};
    vk::DescriptorImageInfo nrdComposeBaseStorageInfo{};
    vk::DescriptorImageInfo nrdComposeIndirectStorageInfo{};
    vk::DescriptorImageInfo nrdComposeBaseSampledInfo{};
    vk::DescriptorImageInfo nrdComposeIndirectSampledInfo{};
    vk::DescriptorImageInfo nrdShadowInInfo{};
    vk::DescriptorImageInfo nrdShadowOutInfo{};
    vk::DescriptorImageInfo blueNoiseInfo{};

    const NrdPerImageResources *nrdResources = nullptr;
    if (!m_nrdPerImage.empty()) {
        const size_t nrdIndex = std::min<size_t>(imageIndex, m_nrdPerImage.size() - 1u);
        nrdResources = &m_nrdPerImage[nrdIndex];
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

        nrdComposeBaseStorageInfo.sampler = VK_NULL_HANDLE;
        nrdComposeBaseStorageInfo.imageView = *nrdResources->composeBase.view;
        nrdComposeBaseStorageInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdComposeIndirectStorageInfo.sampler = VK_NULL_HANDLE;
        nrdComposeIndirectStorageInfo.imageView = *nrdResources->composeIndirect.view;
        nrdComposeIndirectStorageInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdComposeBaseSampledInfo.sampler =
            (m_nrdOutputSampler != nullptr) ? *m_nrdOutputSampler : VK_NULL_HANDLE;
        nrdComposeBaseSampledInfo.imageView = *nrdResources->composeBase.view;
        nrdComposeBaseSampledInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdComposeIndirectSampledInfo.sampler =
            (m_nrdOutputSampler != nullptr) ? *m_nrdOutputSampler : VK_NULL_HANDLE;
        nrdComposeIndirectSampledInfo.imageView = *nrdResources->composeIndirect.view;
        nrdComposeIndirectSampledInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdShadowInInfo.sampler = VK_NULL_HANDLE;
        nrdShadowInInfo.imageView = *nrdResources->shadowIn.view;
        nrdShadowInInfo.imageLayout = vk::ImageLayout::eGeneral;

        nrdShadowOutInfo.sampler =
            (m_nrdOutputSampler != nullptr) ? *m_nrdOutputSampler : VK_NULL_HANDLE;
        nrdShadowOutInfo.imageView = *nrdResources->shadowOut.view;
        nrdShadowOutInfo.imageLayout = vk::ImageLayout::eGeneral;
    } else if (!m_loggedMissingNrdResources) {
        std::cerr << "[Vulkan][NRD] Missing signal resources, GI descriptors are using null NRD "
                     "bindings.\n";
        m_loggedMissingNrdResources = true;
    }

    blueNoiseInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    blueNoiseInfo.imageView = m_giBlueNoiseTexture.getImageView();
    blueNoiseInfo.sampler = m_giBlueNoiseTexture.getSampler();
    if (blueNoiseInfo.imageView == VK_NULL_HANDLE || blueNoiseInfo.sampler == VK_NULL_HANDLE) {
        blueNoiseInfo.imageView = m_fallback2DTexture.getImageView();
        blueNoiseInfo.sampler = m_fallback2DTexture.getSampler();
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(m_giRtDescriptorEnabled ? GI_BINDING_COUNT : (GI_BINDING_COUNT - 1u));
    auto pushBufferWrite =
        [&](uint32_t binding, vk::DescriptorType type, const vk::DescriptorBufferInfo *info) {
            vk::WriteDescriptorSet write{};
            write.dstSet = *m_giDescriptorSets[imageIndex];
            write.dstBinding = binding;
            write.descriptorType = type;
            write.descriptorCount = 1;
            write.pBufferInfo = info;
            writes.push_back(write);
        };
    auto pushImageWrite =
        [&](uint32_t binding, vk::DescriptorType type, const vk::DescriptorImageInfo *info) {
            vk::WriteDescriptorSet write{};
            write.dstSet = *m_giDescriptorSets[imageIndex];
            write.dstBinding = binding;
            write.descriptorType = type;
            write.descriptorCount = 1;
            write.pImageInfo = info;
            writes.push_back(write);
        };
    pushBufferWrite(
        GI_PARAM_BINDING, vk::DescriptorType::eUniformBuffer, &bufferInfos[GI_PARAM_BINDING]
    );
    pushBufferWrite(
        GI_SHADOW_OCCUPANCY_BINDING,
        vk::DescriptorType::eStorageBuffer,
        &bufferInfos[GI_SHADOW_OCCUPANCY_BINDING]
    );
    pushBufferWrite(
        GI_MATERIAL_BINDING, vk::DescriptorType::eStorageBuffer, &bufferInfos[GI_MATERIAL_BINDING]
    );
    pushImageWrite(GI_NRD_DIFF_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdDiffInInfo);
    pushImageWrite(
        GI_NRD_NORMAL_ROUGHNESS_IN_BINDING,
        vk::DescriptorType::eStorageImage,
        &nrdNormalRoughnessInInfo
    );
    pushImageWrite(GI_NRD_MV_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdMvInInfo);
    pushImageWrite(GI_NRD_VIEWZ_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdViewZInInfo);
    pushImageWrite(
        GI_NRD_DIFF_OUT_BINDING, vk::DescriptorType::eCombinedImageSampler, &nrdDiffOutInfo
    );
    pushImageWrite(
        GI_NRD_COMPOSE_BASE_STORAGE_BINDING,
        vk::DescriptorType::eStorageImage,
        &nrdComposeBaseStorageInfo
    );
    pushImageWrite(
        GI_NRD_COMPOSE_INDIRECT_STORAGE_BINDING,
        vk::DescriptorType::eStorageImage,
        &nrdComposeIndirectStorageInfo
    );
    pushImageWrite(
        GI_NRD_COMPOSE_BASE_SAMPLED_BINDING,
        vk::DescriptorType::eCombinedImageSampler,
        &nrdComposeBaseSampledInfo
    );
    pushImageWrite(
        GI_NRD_COMPOSE_INDIRECT_SAMPLED_BINDING,
        vk::DescriptorType::eCombinedImageSampler,
        &nrdComposeIndirectSampledInfo
    );
    pushImageWrite(
        GI_NRD_SHADOW_IN_BINDING, vk::DescriptorType::eStorageImage, &nrdShadowInInfo
    );
    pushImageWrite(
        GI_NRD_SHADOW_OUT_BINDING, vk::DescriptorType::eCombinedImageSampler, &nrdShadowOutInfo
    );
    pushImageWrite(
        GI_BLUE_NOISE_BINDING, vk::DescriptorType::eCombinedImageSampler, &blueNoiseInfo
    );

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
    VulkanUtils::destroyBuffer(m_giFallbackShadowOccupancyBuffer);
    VulkanUtils::destroyBuffer(m_giFallbackMaterialBuffer);
}
