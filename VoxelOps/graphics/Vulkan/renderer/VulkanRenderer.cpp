#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/graphics/Model.hpp"
#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <imgui.h>
#if __has_include(<imgui_impl_vulkan.h>)
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 1
#include <imgui_impl_vulkan.h>
#else
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 0
#endif

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
static_assert(sizeof(IndexedIndirectCommand) == sizeof(vk::DrawIndexedIndirectCommand));
static_assert(alignof(IndexedIndirectCommand) == alignof(vk::DrawIndexedIndirectCommand));

// ReSTIR GI production toggles.
// Keep the legacy GI spatial compute stage disabled until it is rewritten as true reservoir
// resampling.
constexpr bool kRestirGiDispatchSpatialPass = false;
// Use temporal GI buffers as history source (not the legacy spatial GI buffers).
constexpr bool kRestirGiUseSpatialHistory = false;
// Use a single history slot to avoid swapchain-image history divergence/flicker.
constexpr uint32_t kRestirHistorySlot = 0u;

struct alignas(16) GiReservedStorageParamsGpu {
    glm::ivec4 originSpacingBlocks{0};
    glm::uvec4 reservedCounts{0u};
};

struct alignas(16) GiLightingParamsGpu {
    glm::uvec4 header{
        0u}; // x=reservedStorageCount, y=sunShadowEnabled, z=pathTraceEnabled, w=nrdDebugView
    glm::uvec4 pathConfig{
        1u, 0u, 0u, 0u}; // x=raysPerPixel, y=restirHistoryValid, z=frameIndexLow, w=historyReset
    glm::uvec4 tracingConfig{
        0u, 0u, 0u, 0u}; // x=backend(0=dda,1=rt), y=hwRtSupported, z=tlasValid, w=nrdHistoryValid
    glm::vec4 tuning{1.0f, 0.55f, 0.70f,
                     0.00f}; // x=baseDiffuse, y=giIntensity, z=sunIntensity, w=shadowMinVisibility
    glm::vec4 sunDirection{0.25f, 0.85f, 0.42f, 0.0f};
    glm::ivec4 shadowOccupancyMinWordCount{0}; // xyz=min blocks, w=wordCount
    glm::uvec4 shadowOccupancyDims{0u};        // xyz=dims blocks
    glm::ivec4 shadowWorldBoundsXy{0};         // x=minX,y=maxX,z=minY,w=maxY
    glm::ivec4 shadowWorldBoundsZ{0};          // x=minZ,y=maxZ
    glm::vec4 shadowParams{64.0f, 0.08f, 2.0f,
                           1.0f}; // x=maxDistance, y=normalBias, z=maxBounces, w=skyIntensity
    glm::vec4 restirParams{0.00f, 0.00f, 0.0f,
                           0.0f}; // x=temporalBlend, y=spatialReuseWeight, zw=invViewportSize
    glm::vec4 denoiseParams{0.32f, 0.12f, 2.0f,
                            0.24f}; // x=temporalBlend, y=spatialWeight, z=lumaPhi, w=momentBlend
    glm::mat4 currViewProjection{1.0f};
    glm::mat4 prevViewProjection{1.0f};
    glm::mat4 nrdPrevViewProjection{1.0f};
    std::array<GiReservedStorageParamsGpu, 3> reservedStorage{};
};

struct alignas(16) RestirGiSpatialPushConstants {
    glm::uvec2 extent{0u, 0u};
    glm::vec2 invExtent{0.0f, 0.0f};
    float spatialWeight = 0.10f;
    float lumaPhi = 2.0f;
    float normalPower = 12.0f;
    float depthScale = 1200.0f;
};

vk::raii::ShaderModule loadShaderModule(const vk::raii::Device &device, const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open shader: " + path);
    }

    const size_t size = static_cast<size_t>(file.tellg());
    if (size == 0 || (size % 4) != 0) {
        throw std::runtime_error("Invalid shader size: " + path);
    }

    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("Failed to read shader: " + path);
    }

    vk::ShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.codeSize = size;
    shaderModuleInfo.pCode = code.data();
    return vk::raii::ShaderModule(device, shaderModuleInfo);
}

#if VOXELOPS_NRD_HEADERS
vk::raii::ShaderModule loadShaderModuleFromBytes(const vk::raii::Device &device, const void *bytes,
                                                 size_t size) {
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
}

uint32_t divideUp(uint32_t x, uint16_t y) {
    return (x + static_cast<uint32_t>(y) - 1u) / static_cast<uint32_t>(y);
}
#endif

vk::DeviceSize growCapacity(vk::DeviceSize minimum, vk::DeviceSize required) {
    if (required <= minimum) {
        return minimum;
    }

    vk::DeviceSize capacity = minimum;
    while (capacity < required) {
        capacity *= 2;
    }
    return capacity;
}
} // namespace

VulkanRenderer::VulkanRenderer(VulkanContext &context)
    : m_context(context), m_nrdBootstrap(std::make_unique<NrdBootstrap>()) {}

VulkanRenderer::~VulkanRenderer() noexcept {
    try {
        cleanup();
    } catch (const std::exception &e) {
        std::cerr << "VulkanRenderer cleanup failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "VulkanRenderer cleanup failed with unknown exception.\n";
    }
}

vk::RenderPass VulkanRenderer::getRenderPassHandle() const noexcept {
    if (m_renderPass.get() == nullptr) {
        return VK_NULL_HANDLE;
    }
    return *m_renderPass.get();
}

uint32_t VulkanRenderer::getSwapchainImageCount() const noexcept {
    return static_cast<uint32_t>(m_framebuffers.size());
}

bool VulkanRenderer::isNrdBootstrapActive() const noexcept {
    return (m_nrdBootstrap != nullptr) && m_nrdBootstrap->isActive();
}

uint32_t VulkanRenderer::getNrdBootstrapDispatchCount() const noexcept {
    return (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->lastDispatchCount() : 0u;
}

void VulkanRenderer::init() {
    if (m_initialized) {
        cleanup();
    }

    createCommandPool();

    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();
    const vk::raii::Queue &graphicsQueue = m_context.getGraphicsQueue();

    m_uploadContext.init(device, m_context.getGraphicsQueueFamily(), graphicsQueue);

    m_fallbackArrayTexture.initFromAtlasFileAsArray(device, physicalDevice, m_uploadContext, "", 1,
                                                    m_context.isSamplerAnisotropyEnabled(),
                                                    m_context.getMaxSamplerAnisotropy());
    m_fallback2DTexture.initFromFile(device, physicalDevice, m_uploadContext, "",
                                     m_context.isSamplerAnisotropyEnabled(),
                                     m_context.getMaxSamplerAnisotropy());

    m_uploadContext.waitIdle();

    m_frameSync.init(device, MAX_FRAMES_IN_FLIGHT);
    if (m_nrdBootstrap != nullptr) {
        m_nrdBootstrap->init();
    }
    recreateSwapchainDependentResources();

    m_initialized = true;
}

void VulkanRenderer::renderFrame(uint32_t windowWidth, uint32_t windowHeight,
                                 const glm::mat4 &viewMatrix, const glm::mat4 &projectionMatrix,
                                 const glm::mat4 &viewProjection,
                                 const FrameRenderData &frameData) {
    if (!m_initialized || m_framebuffers.empty()) {
        return;
    }

    const vk::Extent2D currentExtent = m_context.getSwapchainExtent();
    if (currentExtent.width == 0 || currentExtent.height == 0) {
        handleWindowResize(windowWidth, windowHeight);
        return;
    }

    m_uploadContext.poll();

    const vk::raii::Device &device = m_context.getDevice();

    const vk::Fence currentFrameFence = m_frameSync.getCurrentFrameFence();
    std::array<vk::Fence, 1> currentFrameFenceArray = {currentFrameFence};
    (void)device.waitForFences(currentFrameFenceArray, vk::True,
                               std::numeric_limits<uint64_t>::max());
    if (frameData.giLighting.pathTracingEnabled && m_restirSharedHistoryFence != VK_NULL_HANDLE &&
        m_restirSharedHistoryFence != currentFrameFence) {
        std::array<vk::Fence, 1> historyFenceArray = {m_restirSharedHistoryFence};
        (void)device.waitForFences(historyFenceArray, vk::True,
                                   std::numeric_limits<uint64_t>::max());
    }

    uint32_t imageIndex = 0;
    vk::Result acquireResult = vk::Result::eSuccess;
    try {
        auto acquire = m_context.getSwapchain().acquireNextImage(
            std::numeric_limits<uint64_t>::max(), m_frameSync.getCurrentImageAvailableSemaphore(),
            nullptr);
        acquireResult = acquire.result;
        imageIndex = acquire.value;
    } catch (const vk::OutOfDateKHRError &) {
        handleWindowResize(windowWidth, windowHeight);
        return;
    }

    if (acquireResult == vk::Result::eErrorOutOfDateKHR) {
        handleWindowResize(windowWidth, windowHeight);
        return;
    }
    if (acquireResult != vk::Result::eSuccess && acquireResult != vk::Result::eSuboptimalKHR) {
        handleWindowResize(windowWidth, windowHeight);
        return;
    }

    const vk::Fence imageFence = m_frameSync.getImageInFlightFence(imageIndex);
    if (imageFence) {
        std::array<vk::Fence, 1> imageFenceArray = {imageFence};
        (void)device.waitForFences(imageFenceArray, vk::True, std::numeric_limits<uint64_t>::max());
        updateGpuTimingStatsForImage(imageIndex);
    } else {
        m_lastFrameTimingStats.gpuValid = false;
    }

    m_frameSync.setImageInFlightFence(imageIndex, currentFrameFence);
    (void)device.resetFences(currentFrameFenceArray);

    std::vector<glm::mat4> combinedModelMatrices = frameData.modelMatrices;
    combinedModelMatrices.reserve(frameData.modelMatrices.size() + frameData.objects.size());
    for (const RenderObject &object : frameData.objects) {
        combinedModelMatrices.emplace_back(object.transform);
    }
    uint32_t requiredIndirectMatrixCount = 0;
    for (const IndexedIndirectCommand &command : frameData.indirectCommands) {
        if (command.instanceCount == 0) {
            continue;
        }

        const uint64_t commandEnd = static_cast<uint64_t>(command.firstInstance) +
                                    static_cast<uint64_t>(command.instanceCount);
        if (commandEnd > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            requiredIndirectMatrixCount = std::numeric_limits<uint32_t>::max();
            break;
        }

        requiredIndirectMatrixCount =
            std::max(requiredIndirectMatrixCount, static_cast<uint32_t>(commandEnd));
    }
    if (combinedModelMatrices.size() < requiredIndirectMatrixCount) {
        combinedModelMatrices.resize(requiredIndirectMatrixCount, glm::mat4(1.0f));
    }

    updatePerImageDrawBuffers(imageIndex, combinedModelMatrices, frameData.indirectCommands);
    updateGiDescriptorSet(imageIndex, frameData, viewProjection);
    if (m_nrdBootstrap != nullptr) {
        glm::mat4 prevViewMatrix = viewMatrix;
        glm::mat4 prevProjectionMatrix = projectionMatrix;
        bool hasPrevMatrices = frameData.giLighting.enabled && !frameData.giLighting.resetHistory &&
                               m_nrdPrevMatricesValid;
        if (hasPrevMatrices) {
            prevViewMatrix = m_nrdPrevView;
            prevProjectionMatrix = m_nrdPrevProjection;
        }
        m_nrdBootstrap->updateFrame(viewMatrix, projectionMatrix, prevViewMatrix,
                                    prevProjectionMatrix, hasPrevMatrices, frameData,
                                    currentExtent.width, currentExtent.height);
    }

    m_commandBuffers[imageIndex].reset();
    float chunkCpuMs = 0.0f;
    float modelCpuMs = 0.0f;
    float uiCpuMs = 0.0f;
    const auto cpuRecordStart = std::chrono::steady_clock::now();
    recordCommandBuffer(imageIndex, viewProjection, frameData, chunkCpuMs, modelCpuMs, uiCpuMs);
    const auto cpuRecordEnd = std::chrono::steady_clock::now();
    m_lastFrameTimingStats.cpuCommandRecordMs = static_cast<float>(
        std::chrono::duration<double, std::milli>(cpuRecordEnd - cpuRecordStart).count());
    m_lastFrameTimingStats.cpuChunkPassMs = chunkCpuMs;
    m_lastFrameTimingStats.cpuModelPassMs = modelCpuMs;
    m_lastFrameTimingStats.cpuUiPassMs = uiCpuMs;

    const vk::Semaphore waitSemaphore = m_frameSync.getCurrentImageAvailableSemaphore();
    const vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    const vk::CommandBuffer commandBuffer = *m_commandBuffers[imageIndex];
    const vk::Semaphore signalSemaphore = m_frameSync.getRenderFinishedSemaphore(imageIndex);

    vk::SubmitInfo submitInfo{};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &waitSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;

    m_context.getGraphicsQueue().submit(submitInfo, currentFrameFence);
    m_restirSharedHistoryFence =
        frameData.giLighting.pathTracingEnabled ? currentFrameFence : VK_NULL_HANDLE;

    const uint32_t historyIndex = kRestirHistorySlot;
    if (historyIndex < m_restirDiValidPerImage.size()) {
        if (frameData.giLighting.pathTracingEnabled) {
            m_restirDiValidPerImage[historyIndex] = true;
            if (historyIndex < m_restirDiWriteParityPerImage.size()) {
                m_restirDiWriteParityPerImage[historyIndex] ^= 1u;
            }
        } else {
            m_restirDiValidPerImage[historyIndex] = false;
        }
    }
    if (historyIndex < m_restirDiPrevViewProjectionPerImage.size() &&
        historyIndex < m_prevViewProjectionValidPerImage.size() &&
        historyIndex < m_restirDiPrevViewPerImage.size() &&
        historyIndex < m_restirDiPrevProjectionPerImage.size()) {
        if (frameData.giLighting.enabled) {
            m_restirDiPrevViewProjectionPerImage[historyIndex] = viewProjection;
            m_restirDiPrevViewPerImage[historyIndex] = viewMatrix;
            m_restirDiPrevProjectionPerImage[historyIndex] = projectionMatrix;
            m_prevViewProjectionValidPerImage[historyIndex] = true;
        } else {
            m_prevViewProjectionValidPerImage[historyIndex] = false;
        }
    }
    if (frameData.giLighting.enabled) {
        m_nrdPrevViewProjection = viewProjection;
        m_nrdPrevView = viewMatrix;
        m_nrdPrevProjection = projectionMatrix;
        m_nrdPrevMatricesValid = true;
    } else {
        m_nrdPrevMatricesValid = false;
    }

    ++m_frameCounterLow;

    const vk::SwapchainKHR swapchain = *m_context.getSwapchain();
    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &signalSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    try {
        const vk::Result presentResult = m_context.getPresentQueue().presentKHR(presentInfo);
        if (m_context.shouldRecreateSwapchain(presentResult)) {
            handleWindowResize(windowWidth, windowHeight);
        }
    } catch (const vk::OutOfDateKHRError &) {
        handleWindowResize(windowWidth, windowHeight);
    }

    m_frameSync.advanceFrame(MAX_FRAMES_IN_FLIGHT);
}

void VulkanRenderer::handleWindowResize(uint32_t windowWidth, uint32_t windowHeight) {
    if (!m_initialized || windowWidth == 0 || windowHeight == 0) {
        return;
    }

    if (!m_context.handleWindowResize(windowWidth, windowHeight)) {
        return;
    }

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0) {
        return;
    }

    recreateSwapchainDependentResources();
}

void VulkanRenderer::cleanup() {
    if (!m_initialized) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    try {
        device.waitIdle();
    } catch (const std::exception &e) {
        std::cerr << "VulkanRenderer::cleanup waitIdle failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "VulkanRenderer::cleanup waitIdle failed with unknown exception.\n";
    }

    cleanupSwapchainDependentResources();
    m_frameSync.cleanup();
    if (m_nrdBootstrap != nullptr) {
        m_nrdBootstrap->shutdown();
    }
    m_fallbackArrayTexture.cleanup();
    m_fallback2DTexture.cleanup();
    m_uploadContext.cleanup();

    m_commandPool.clear();
    m_restirSharedHistoryFence = VK_NULL_HANDLE;
    m_initialized = false;
}

void VulkanRenderer::createCommandPool() {
    const vk::raii::Device &device = m_context.getDevice();
    if (m_commandPool != nullptr) {
        return;
    }

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_context.getGraphicsQueueFamily();
    m_commandPool = vk::raii::CommandPool(device, poolInfo);
}

void VulkanRenderer::createCommandBuffers() {
    const vk::raii::Device &device = m_context.getDevice();
    m_commandBuffers.clear();

    if (m_framebuffers.empty()) {
        return;
    }

    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = *m_commandPool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_framebuffers.size());
    m_commandBuffers = device.allocateCommandBuffers(allocInfo);
}

void VulkanRenderer::createFramebuffers() {
    const vk::raii::Device &device = m_context.getDevice();
    const auto &swapchainImageViews = m_context.getSwapchainImageViews();
    const vk::Extent2D extent = m_context.getSwapchainExtent();

    m_framebuffers.clear();
    m_framebuffers.reserve(swapchainImageViews.size());

    for (const auto &swapchainImageView : swapchainImageViews) {
        std::array<vk::ImageView, 2> attachments = {*swapchainImageView,
                                                    *m_context.getDepthImageView()};

        vk::FramebufferCreateInfo framebufferInfo{};
        framebufferInfo.renderPass = *m_renderPass.get();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        m_framebuffers.emplace_back(device, framebufferInfo);
    }
}

void VulkanRenderer::createModelDescriptorResources() {
    cleanupModelDescriptorResources();

    if (m_framebuffers.empty()) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();

    vk::DescriptorSetLayoutBinding storageBinding{};
    storageBinding.binding = 0;
    storageBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
    storageBinding.descriptorCount = 1;
    storageBinding.stageFlags = vk::ShaderStageFlagBits::eVertex;

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &storageBinding;
    m_modelDescriptorSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);

    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eStorageBuffer;
    poolSize.descriptorCount = static_cast<uint32_t>(m_framebuffers.size());

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = static_cast<uint32_t>(m_framebuffers.size());
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    m_modelDescriptorPool = vk::raii::DescriptorPool(device, poolInfo);

    std::vector<vk::DescriptorSetLayout> layouts(m_framebuffers.size(),
                                                 *m_modelDescriptorSetLayout);
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *m_modelDescriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    m_modelDescriptorSets = device.allocateDescriptorSets(allocateInfo);

    m_perImageDrawResources.clear();
    m_perImageDrawResources.resize(m_framebuffers.size());
}

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

void VulkanRenderer::createNrdSignalResources() {
    cleanupNrdSignalResources();

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
    m_nrdPerImage.resize(1);
    m_nrdValidPerImage.assign(imageCount, false);

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;
    constexpr vk::Format kDiffFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kNormalRoughnessFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kMotionFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kViewZFormat = vk::Format::eR32Sfloat;
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
        return static_cast<bool>(props.optimalTilingFeatures &
                                 vk::FormatFeatureFlagBits::eStorageImage);
    };
    if (!supportsStorage(kDiffFormat) || !supportsStorage(kNormalRoughnessFormat) ||
        !supportsStorage(kMotionFormat) || !supportsStorage(kViewZFormat)) {
        throw std::runtime_error(
            "NRD signal formats are not supported as storage images by this Vulkan device.");
    }

    const auto createSignal = [&](RestirDiReservoirResources &dst, vk::Format format, uint32_t w,
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
            physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
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
    m_nrdFallbackReady = true;

    NrdPerImageResources &resources = m_nrdPerImage[0];
    createSignal(resources.diffIn, kDiffFormat, extent.width, extent.height);
    createSignal(resources.normalRoughnessIn, kNormalRoughnessFormat, extent.width, extent.height);
    createSignal(resources.motionIn, kMotionFormat, extent.width, extent.height);
    createSignal(resources.viewZIn, kViewZFormat, extent.width, extent.height);
    createSignal(resources.diffOut, kDiffFormat, extent.width, extent.height);

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
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                      vk::PipelineStageFlagBits::eTransfer |
                                          vk::PipelineStageFlagBits::eFragmentShader |
                                          vk::PipelineStageFlagBits::eComputeShader,
                                      {}, {}, {}, toGeneral);
        commandBuffer.clearColorImage(image, vk::ImageLayout::eGeneral, clearValue, range);
    };

    const vk::ClearColorValue clearZero(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearNormalRoughness(std::array<float, 4>{0.5f, 1.0f, 0.5f, 1.0f});
    const vk::ClearColorValue clearViewZ(std::array<float, 4>{-1.0f, 0.0f, 0.0f, 0.0f});
    transitionAndClear(*m_nrdFallback.diffIn.image, clearZero);
    transitionAndClear(*m_nrdFallback.normalRoughnessIn.image, clearNormalRoughness);
    transitionAndClear(*m_nrdFallback.motionIn.image, clearZero);
    transitionAndClear(*m_nrdFallback.viewZIn.image, clearViewZ);
    transitionAndClear(*m_nrdFallback.diffOut.image, clearZero);
    transitionAndClear(*resources.diffIn.image, clearZero);
    transitionAndClear(*resources.normalRoughnessIn.image, clearNormalRoughness);
    transitionAndClear(*resources.motionIn.image, clearZero);
    transitionAndClear(*resources.viewZIn.image, clearViewZ);
    transitionAndClear(*resources.diffOut.image, clearZero);

    VulkanUtils::endSingleTimeCommands(device, m_context.getGraphicsQueue(),
                                       std::move(commandBuffer));
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
            bindings.reserve(static_cast<size_t>(m_nrdTextureCapacity) +
                             static_cast<size_t>(m_nrdStorageCapacity));

            const auto pushBinding = [&](uint32_t binding, vk::DescriptorType type) {
                for (const vk::DescriptorSetLayoutBinding &existing : bindings) {
                    if (existing.binding != binding) {
                        continue;
                    }
                    if (existing.descriptorType != type) {
                        throw std::runtime_error(
                            "NRD descriptor binding collision in resources set layout.");
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
            std::sort(bindings.begin(), bindings.end(),
                      [](const auto &a, const auto &b) { return a.binding < b.binding; });

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
                device, pipelineDesc.computeShaderSPIRV.bytecode,
                static_cast<size_t>(pipelineDesc.computeShaderSPIRV.size));
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
                throw std::runtime_error("NRD sampled format " + formatId +
                                         " is not supported by this Vulkan device.");
            }
            if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage) ==
                vk::FormatFeatureFlags()) {
                throw std::runtime_error("NRD storage format " + formatId +
                                         " is not supported by this Vulkan device.");
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
            allocInfo.memoryTypeIndex =
                VulkanUtils::findMemoryType(physicalDevice, requirements.memoryTypeBits,
                                            vk::MemoryPropertyFlagBits::eDeviceLocal);
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

        m_nrdRuntimePerImage.resize(1);
        NrdRuntimePerImage &runtime = m_nrdRuntimePerImage[0];
        runtime.permanentPool.resize(instanceDesc->permanentPoolSize);
        runtime.transientPool.resize(instanceDesc->transientPoolSize);
        for (uint32_t i = 0; i < instanceDesc->permanentPoolSize; ++i) {
            createRuntimeTexture(runtime.permanentPool[i], instanceDesc->permanentPool[i]);
        }
        for (uint32_t i = 0; i < instanceDesc->transientPoolSize; ++i) {
            createRuntimeTexture(runtime.transientPool[i], instanceDesc->transientPool[i]);
        }

        const uint32_t maxDispatchSets = std::max(
            64u, std::max(instanceDesc->descriptorPoolDesc.setsMaxNum, instanceDesc->pipelinesNum));
        const uint32_t maxConstantsSets = maxDispatchSets;
        const vk::DeviceSize constantBufferBytes =
            static_cast<vk::DeviceSize>(m_nrdConstantBufferStride) *
            static_cast<vk::DeviceSize>(maxConstantsSets);
        VulkanUtils::createBuffer(
            device, physicalDevice, constantBufferBytes, vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            runtime.constantBuffer, runtime.constantMemory);
        runtime.constantMapped = runtime.constantMemory.mapMemory(0, constantBufferBytes);

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
        runtime.descriptorPool = vk::raii::DescriptorPool(device, poolInfo);

        std::vector<vk::DescriptorSetLayout> layouts(maxDispatchSets + 1u,
                                                     *m_nrdResourcesSetLayout);
        layouts[maxDispatchSets] = *m_nrdConstantsSetLayout;
        vk::DescriptorSetAllocateInfo allocInfo{};
        allocInfo.descriptorPool = *runtime.descriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocInfo.pSetLayouts = layouts.data();
        std::vector<vk::raii::DescriptorSet> sets = device.allocateDescriptorSets(allocInfo);
        runtime.resourcesSets.clear();
        runtime.resourcesSets.reserve(maxDispatchSets);
        for (uint32_t setIndex = 0; setIndex < maxDispatchSets; ++setIndex) {
            runtime.resourcesSets.push_back(std::move(sets[setIndex]));
        }
        runtime.constantsSet = std::move(sets[maxDispatchSets]);

        vk::DescriptorBufferInfo constantInfo{};
        constantInfo.buffer = *runtime.constantBuffer;
        constantInfo.offset = 0;
        constantInfo.range = m_nrdConstantBufferSize;
        vk::WriteDescriptorSet constantWrite{};
        constantWrite.dstSet = *runtime.constantsSet;
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

            samplerWrites[i].dstSet = *runtime.constantsSet;
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
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                              vk::PipelineStageFlagBits::eComputeShader, {}, {}, {},
                                              barrier);
            }
        };
        transitionPool(runtime.permanentPool);
        transitionPool(runtime.transientPool);
        VulkanUtils::endSingleTimeCommands(device, m_context.getGraphicsQueue(),
                                           std::move(commandBuffer));

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
        if (runtime.constantMapped != nullptr) {
            runtime.constantMemory.unmapMemory();
            runtime.constantMapped = nullptr;
        }
        runtime.resourcesSets.clear();
        runtime.constantsSet.clear();
        runtime.descriptorPool.clear();
        runtime.constantBuffer.clear();
        runtime.constantMemory.clear();
        runtime.permanentPool.clear();
        runtime.transientPool.clear();
    }
    m_nrdRuntimePerImage.clear();
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

void VulkanRenderer::dispatchNrdPass(uint32_t imageIndex, const FrameRenderData &frameData) {
    const auto setNrdValidForAllImages = [&](bool valid) {
        std::fill(m_nrdValidPerImage.begin(), m_nrdValidPerImage.end(), valid);
    };

    if (!m_nrdRuntimeReady || m_nrdBootstrap == nullptr || !m_nrdBootstrap->isActive() ||
        m_nrdRuntimePerImage.empty() || m_nrdPerImage.empty()) {
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
        setNrdValidForAllImages(false);
        return;
    }

    NrdRuntimePerImage &runtime = m_nrdRuntimePerImage[0];
    const NrdPerImageResources &external = m_nrdPerImage[0];
    if (runtime.constantMapped == nullptr || runtime.constantsSet == nullptr ||
        runtime.resourcesSets.empty()) {
        setNrdValidForAllImages(false);
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

    const uint32_t maxConstantsSets = static_cast<uint32_t>(runtime.resourcesSets.size());
    uint32_t constantSetIndex = 0;
    bool loggedResourceSetOverflow = false;
    bool dispatchedAll = true;
    for (uint32_t i = 0; i < dispatchCount; ++i) {
        if (i >= runtime.resourcesSets.size()) {
            if (!loggedResourceSetOverflow) {
                std::cerr << "[Vulkan][NRD] Insufficient per-dispatch descriptor sets: required="
                          << dispatchCount << ", allocated=" << runtime.resourcesSets.size()
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
        const vk::DescriptorSet resourceSetForDispatch = *runtime.resourcesSets[i];
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
                static_cast<uint8_t *>(runtime.constantMapped) +
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

        m_commandBuffers[imageIndex].bindPipeline(vk::PipelineBindPoint::eCompute,
                                                  *m_nrdPipelines[dispatch.pipelineIndex]);
        const vk::DescriptorSet resourcesSet = resourceSetForDispatch;
        m_commandBuffers[imageIndex].bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                                        *m_nrdPipelineLayout,
                                                        m_nrdSetResourcesIndex, resourcesSet, {});
        const vk::DescriptorSet constantsSet = *runtime.constantsSet;
        m_commandBuffers[imageIndex].bindDescriptorSets(
            vk::PipelineBindPoint::eCompute, *m_nrdPipelineLayout, m_nrdSetConstantsIndex,
            constantsSet, dynamicOffsets);
        m_commandBuffers[imageIndex].dispatch(dispatch.gridWidth, dispatch.gridHeight, 1u);

        vk::MemoryBarrier barrier{};
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                                     vk::PipelineStageFlagBits::eComputeShader, {},
                                                     barrier, {}, {});
    }

    setNrdValidForAllImages(dispatchedAll);
}
#endif

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
                allocInfo.memoryTypeIndex =
                    VulkanUtils::findMemoryType(physicalDevice, requirements.memoryTypeBits,
                                                vk::MemoryPropertyFlagBits::eDeviceLocal);
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
                allocInfo.memoryTypeIndex =
                    VulkanUtils::findMemoryType(physicalDevice, requirements.memoryTypeBits,
                                                vk::MemoryPropertyFlagBits::eDeviceLocal);
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
        temporalGiBinding, temporalGiMetaBinding, temporalValidationBinding, spatialGiBinding,
        spatialGiMetaBinding};

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
        loadShaderModule(device, shaderDir + "restir_gi_spatial.comp.spv");

    vk::PushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pushConstantRange.offset = 0;
    pushConstantRange.size = static_cast<uint32_t>(sizeof(RestirGiSpatialPushConstants));

    const std::array<vk::DescriptorSetLayout, 1> computeLayouts = {
        *m_restirGiSpatialDescriptorSetLayout};
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
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                          vk::PipelineStageFlagBits::eTransfer |
                                              vk::PipelineStageFlagBits::eFragmentShader |
                                              vk::PipelineStageFlagBits::eComputeShader,
                                          {}, {}, {}, toGeneral);
            commandBuffer.clearColorImage(*reservoir.image, vk::ImageLayout::eGeneral, clearZero,
                                          range);
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
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                          vk::PipelineStageFlagBits::eTransfer |
                                              vk::PipelineStageFlagBits::eFragmentShader |
                                              vk::PipelineStageFlagBits::eComputeShader,
                                          {}, {}, {}, toGeneral);
            commandBuffer.clearColorImage(*meta.image, vk::ImageLayout::eGeneral, clearMeta, range);
        }
    }
    VulkanUtils::endSingleTimeCommands(device, m_context.getGraphicsQueue(),
                                       std::move(commandBuffer));
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

void VulkanRenderer::dispatchRestirGiSpatialPass(uint32_t imageIndex,
                                                 const FrameRenderData &frameData) {
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
        vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, barriers);

    m_commandBuffers[imageIndex].bindPipeline(vk::PipelineBindPoint::eCompute,
                                              *m_restirGiSpatialPipeline);
    const vk::DescriptorSet descriptorSet = *m_restirGiSpatialDescriptorSets[imageIndex];
    m_commandBuffers[imageIndex].bindDescriptorSets(
        vk::PipelineBindPoint::eCompute, *m_restirGiSpatialPipelineLayout, 0, descriptorSet, {});

    RestirGiSpatialPushConstants push{};
    push.extent = glm::uvec2(extent.width, extent.height);
    push.invExtent = glm::vec2(1.0f / static_cast<float>(extent.width),
                               1.0f / static_cast<float>(extent.height));
    push.spatialWeight = glm::clamp(frameData.giLighting.restirSpatialReuse, 0.0f, 0.35f);
    push.lumaPhi = std::max(frameData.giLighting.denoiseLumaPhi, 0.05f);
    push.normalPower = 10.0f;
    push.depthScale = 24.0f;
    m_commandBuffers[imageIndex].pushConstants<RestirGiSpatialPushConstants>(
        *m_restirGiSpatialPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, push);

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
            addTarget(*m_restirValidationPerImage[historyIndex][writeParity].image,
                      clearValidation);
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
        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, toTransfer);

    for (const ClearTarget &target : targets) {
        m_commandBuffers[imageIndex].clearColorImage(target.image, vk::ImageLayout::eGeneral,
                                                     target.clear, range);
    }

    m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                                 vk::PipelineStageFlagBits::eFragmentShader |
                                                     vk::PipelineStageFlagBits::eComputeShader,
                                                 {}, {}, {}, toShader);
}

void VulkanRenderer::barrierNrdSignalsForCompute(uint32_t imageIndex) {
    if (m_nrdPerImage.empty()) {
        return;
    }

    const NrdPerImageResources &nrd = m_nrdPerImage[0];
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    auto makeBarrier = [&](vk::Image image) {
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
        makeBarrier(*nrd.diffIn.image),   makeBarrier(*nrd.normalRoughnessIn.image),
        makeBarrier(*nrd.motionIn.image), makeBarrier(*nrd.viewZIn.image),
        makeBarrier(*nrd.diffOut.image),
    };
    m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                 vk::PipelineStageFlagBits::eComputeShader, {}, {},
                                                 {}, barriers);
}

void VulkanRenderer::createTimestampResources() {
    cleanupTimestampResources();

    m_timestampQueriesEnabled = m_context.areTimestampQueriesSupported();
    m_timestampPeriodNanoseconds = m_context.getTimestampPeriodNanoseconds();
    if (!m_timestampQueriesEnabled || m_timestampPeriodNanoseconds <= 0.0f ||
        m_framebuffers.empty()) {
        m_timestampQueriesEnabled = false;
        m_timestampPeriodNanoseconds = 0.0f;
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    m_timestampQueryPools.reserve(m_framebuffers.size());
    for (size_t i = 0; i < m_framebuffers.size(); ++i) {
        vk::QueryPoolCreateInfo queryPoolInfo{};
        queryPoolInfo.queryType = vk::QueryType::eTimestamp;
        queryPoolInfo.queryCount = TIMESTAMP_QUERY_COUNT;
        m_timestampQueryPools.emplace_back(device, queryPoolInfo);
    }
}

void VulkanRenderer::cleanupTimestampResources() {
    m_timestampQueryPools.clear();
    m_timestampQueriesEnabled = false;
    m_timestampPeriodNanoseconds = 0.0f;
    m_lastFrameTimingStats.gpuValid = false;
}

void VulkanRenderer::updateGpuTimingStatsForImage(uint32_t imageIndex) {
    if (!m_timestampQueriesEnabled || imageIndex >= m_timestampQueryPools.size()) {
        m_lastFrameTimingStats.gpuValid = false;
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    const vk::QueryPool queryPool = *m_timestampQueryPools[imageIndex];
    std::array<uint64_t, TIMESTAMP_QUERY_COUNT> ticks{};
    const VkResult result =
        vkGetQueryPoolResults(static_cast<VkDevice>(*device), static_cast<VkQueryPool>(queryPool),
                              0, TIMESTAMP_QUERY_COUNT, sizeof(ticks), ticks.data(),
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (result != VK_SUCCESS) {
        m_lastFrameTimingStats.gpuValid = false;
        return;
    }

    const double tickToMs = static_cast<double>(m_timestampPeriodNanoseconds) * 1.0e-6;
    const auto deltaMs = [tickToMs](uint64_t startTick, uint64_t endTick) -> float {
        if (endTick < startTick) {
            return 0.0f;
        }
        return static_cast<float>(static_cast<double>(endTick - startTick) * tickToMs);
    };

    m_lastFrameTimingStats.gpuValid = true;
    m_lastFrameTimingStats.gpuChunkPassMs = deltaMs(ticks[0], ticks[1]);
    m_lastFrameTimingStats.gpuModelPassMs = deltaMs(ticks[1], ticks[2]);
    m_lastFrameTimingStats.gpuUiPassMs = deltaMs(ticks[2], ticks[3]);
    m_lastFrameTimingStats.gpuFrameMs = deltaMs(ticks[0], ticks[4]);
}

void VulkanRenderer::ensurePerImageDrawBufferCapacity(uint32_t imageIndex,
                                                      vk::DeviceSize modelBytes,
                                                      vk::DeviceSize indirectBytes) {
    if (imageIndex >= m_perImageDrawResources.size()) {
        return;
    }

    PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();

    const vk::DeviceSize requiredModelBytes = std::max(modelBytes, MIN_MODEL_BUFFER_BYTES);
    if (resources.modelMatrixCapacityBytes < requiredModelBytes ||
        resources.modelMatrixBuffer == nullptr) {
        if (resources.modelMatrixMapped != nullptr) {
            resources.modelMatrixBufferMemory.unmapMemory();
            resources.modelMatrixMapped = nullptr;
        }
        resources.modelMatrixBuffer.clear();
        resources.modelMatrixBufferMemory.clear();
        resources.modelMatrixCapacityBytes =
            growCapacity(MIN_MODEL_BUFFER_BYTES, requiredModelBytes);

        VulkanUtils::createBuffer(device, physicalDevice, resources.modelMatrixCapacityBytes,
                                  vk::BufferUsageFlagBits::eStorageBuffer,
                                  vk::MemoryPropertyFlagBits::eHostVisible |
                                      vk::MemoryPropertyFlagBits::eHostCoherent,
                                  resources.modelMatrixBuffer, resources.modelMatrixBufferMemory);
        resources.modelMatrixMapped = resources.modelMatrixBufferMemory.mapMemory(0, VK_WHOLE_SIZE);

        updateModelDescriptorSet(imageIndex);
    }

    const vk::DeviceSize requiredIndirectBytes = std::max(indirectBytes, MIN_INDIRECT_BUFFER_BYTES);
    if (resources.indirectCommandCapacityBytes < requiredIndirectBytes ||
        resources.indirectCommandBuffer == nullptr) {
        if (resources.indirectCommandMapped != nullptr) {
            resources.indirectCommandBufferMemory.unmapMemory();
            resources.indirectCommandMapped = nullptr;
        }
        resources.indirectCommandBuffer.clear();
        resources.indirectCommandBufferMemory.clear();
        resources.indirectCommandCapacityBytes =
            growCapacity(MIN_INDIRECT_BUFFER_BYTES, requiredIndirectBytes);

        VulkanUtils::createBuffer(
            device, physicalDevice, resources.indirectCommandCapacityBytes,
            vk::BufferUsageFlagBits::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            resources.indirectCommandBuffer, resources.indirectCommandBufferMemory);
        resources.indirectCommandMapped =
            resources.indirectCommandBufferMemory.mapMemory(0, VK_WHOLE_SIZE);
    }
}

void VulkanRenderer::updatePerImageDrawBuffers(
    uint32_t imageIndex, const std::vector<glm::mat4> &modelMatrices,
    const std::vector<IndexedIndirectCommand> &indirectCommands) {
    if (imageIndex >= m_perImageDrawResources.size()) {
        return;
    }

    const vk::DeviceSize modelBytes =
        static_cast<vk::DeviceSize>(modelMatrices.size() * sizeof(glm::mat4));
    const vk::DeviceSize indirectBytes =
        static_cast<vk::DeviceSize>(indirectCommands.size() * sizeof(IndexedIndirectCommand));
    ensurePerImageDrawBufferCapacity(imageIndex, modelBytes, indirectBytes);

    PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];

    if (!modelMatrices.empty() && resources.modelMatrixMapped != nullptr) {
        std::memcpy(resources.modelMatrixMapped, modelMatrices.data(),
                    static_cast<size_t>(modelBytes));
    }

    if (!indirectCommands.empty() && resources.indirectCommandMapped != nullptr) {
        std::memcpy(resources.indirectCommandMapped, indirectCommands.data(),
                    static_cast<size_t>(indirectBytes));
    }
}

void VulkanRenderer::updateModelDescriptorSet(uint32_t imageIndex) {
    if (imageIndex >= m_perImageDrawResources.size() ||
        imageIndex >= m_modelDescriptorSets.size()) {
        return;
    }

    const PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
    if (resources.modelMatrixBuffer == nullptr || resources.modelMatrixCapacityBytes == 0) {
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();

    vk::DescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = *resources.modelMatrixBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = resources.modelMatrixCapacityBytes;

    vk::WriteDescriptorSet write{};
    write.dstSet = *m_modelDescriptorSets[imageIndex];
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    const std::array<vk::WriteDescriptorSet, 1> writes = {write};
    device.updateDescriptorSets(writes, {});
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

void VulkanRenderer::cleanupPerImageDrawResources() {
    for (PerImageDrawResources &resources : m_perImageDrawResources) {
        if (resources.modelMatrixMapped != nullptr) {
            resources.modelMatrixBufferMemory.unmapMemory();
            resources.modelMatrixMapped = nullptr;
        }
        resources.modelMatrixBuffer.clear();
        resources.modelMatrixBufferMemory.clear();
        resources.modelMatrixCapacityBytes = 0;

        if (resources.indirectCommandMapped != nullptr) {
            resources.indirectCommandBufferMemory.unmapMemory();
            resources.indirectCommandMapped = nullptr;
        }
        resources.indirectCommandBuffer.clear();
        resources.indirectCommandBufferMemory.clear();
        resources.indirectCommandCapacityBytes = 0;

        if (resources.giParamsMapped != nullptr) {
            resources.giParamsBufferMemory.unmapMemory();
            resources.giParamsMapped = nullptr;
        }
        resources.giParamsBuffer.clear();
        resources.giParamsBufferMemory.clear();
    }
    m_perImageDrawResources.clear();
}

void VulkanRenderer::cleanupModelDescriptorResources() {
    m_modelDescriptorSets.clear();
    m_modelDescriptorPool.clear();
    m_modelDescriptorSetLayout.clear();
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

void VulkanRenderer::recreateSwapchainDependentResources() {
    cleanupSwapchainDependentResources();

    const vk::raii::Device &device = m_context.getDevice();

    m_renderPass.create(device, m_context.getSwapchainImageFormat(), m_context.getDepthFormat());
    createFramebuffers();
    createModelDescriptorResources();
    createGiDescriptorResources();
    createRestirDiResources();
    createRestirValidationResources();
    createRestirMetaResources();
    createRestirGiResources();
    createNrdSignalResources();
#if VOXELOPS_NRD_HEADERS
    createNrdRuntimeResources();
#endif
    createRestirGiSpatialResources();
    const bool useRtShaderVariants = m_context.isHardwareRayTracingSupported();
    const char *chunkFragShader =
        useRtShaderVariants ? "triangle_rt.frag.spv" : "triangle.frag.spv";
    const char *modelFragShader = useRtShaderVariants ? "model_rt.frag.spv" : "model.frag.spv";
    m_chunkPipeline.create(device, m_renderPass.get(),
                           m_fallbackArrayTexture.getDescriptorSetLayout(),
                           m_modelDescriptorSetLayout, m_giDescriptorSetLayout,
                           PipelineVertexLayout::PackedVoxel, "triangle.vert.spv", chunkFragShader);
    m_modelPipeline.create(device, m_renderPass.get(), m_fallback2DTexture.getDescriptorSetLayout(),
                           m_modelDescriptorSetLayout, m_giDescriptorSetLayout,
                           PipelineVertexLayout::ModelPosUv, "model.vert.spv", modelFragShader);
    createCommandBuffers();
    createTimestampResources();
    m_frameSync.recreateSwapchainSync(device, m_framebuffers.size());

#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_SetMinImageCount(2);
        ImGui_ImplVulkan_PipelineInfo pipelineInfo{};
        pipelineInfo.RenderPass = *m_renderPass.get();
        pipelineInfo.Subpass = 0;
        pipelineInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_CreateMainPipeline(&pipelineInfo);
    }
#endif
}

void VulkanRenderer::cleanupSwapchainDependentResources() {
    cleanupTimestampResources();
    m_commandBuffers.clear();
    m_framebuffers.clear();
    m_chunkPipeline.cleanup();
    m_modelPipeline.cleanup();
    cleanupPerImageDrawResources();
    cleanupModelDescriptorResources();
    cleanupGiDescriptorResources();
    cleanupRestirDiResources();
    cleanupRestirValidationResources();
    cleanupRestirMetaResources();
    cleanupRestirGiResources();
#if VOXELOPS_NRD_HEADERS
    cleanupNrdRuntimeResources();
#endif
    cleanupNrdSignalResources();
    cleanupRestirGiSpatialResources();
    m_renderPass.cleanup();
}

void VulkanRenderer::recordCommandBuffer(uint32_t imageIndex, const glm::mat4 &viewProjection,
                                         const FrameRenderData &frameData, float &outChunkCpuMs,
                                         float &outModelCpuMs, float &outUiCpuMs) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };
    outChunkCpuMs = 0.0f;
    outModelCpuMs = 0.0f;
    outUiCpuMs = 0.0f;

    vk::CommandBufferBeginInfo beginInfo{};
    m_commandBuffers[imageIndex].begin(beginInfo);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].resetQueryPool(*m_timestampQueryPools[imageIndex], 0,
                                                    TIMESTAMP_QUERY_COUNT);
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 0);
    }
    clearTemporalGiWriteTargets(imageIndex);
    const uint32_t historyIndex = kRestirHistorySlot;

    if (historyIndex < m_restirDiPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirDiPerImage[historyIndex];

        const vk::ImageSubresourceRange reservoirRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> reservoirBarriers{};
        reservoirBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[0].image = *perImage[readParity].image;
        reservoirBarriers[0].subresourceRange = reservoirRange;
        reservoirBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        reservoirBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        reservoirBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[1].image = *perImage[writeParity].image;
        reservoirBarriers[1].subresourceRange = reservoirRange;
        reservoirBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        reservoirBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, reservoirBarriers);
    }

    if (historyIndex < m_restirValidationPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirValidationPerImage[historyIndex];

        const vk::ImageSubresourceRange validationRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                                        1);
        std::array<vk::ImageMemoryBarrier, 2> validationBarriers{};
        validationBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        validationBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        validationBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[0].image = *perImage[readParity].image;
        validationBarriers[0].subresourceRange = validationRange;
        validationBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        validationBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        validationBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        validationBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        validationBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[1].image = *perImage[writeParity].image;
        validationBarriers[1].subresourceRange = validationRange;
        validationBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        validationBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, validationBarriers);
    }

    if (historyIndex < m_restirMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange metaRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> metaBarriers{};
        metaBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        metaBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        metaBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[0].image = *perImage[readParity].image;
        metaBarriers[0].subresourceRange = metaRange;
        metaBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        metaBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        metaBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        metaBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        metaBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[1].image = *perImage[writeParity].image;
        metaBarriers[1].subresourceRange = metaRange;
        metaBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        metaBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, metaBarriers);
    }

    if (historyIndex < m_restirGiPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiPerImage[historyIndex];

        const vk::ImageSubresourceRange giRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> giBarriers{};
        giBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        giBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        giBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[0].image = *perImage[readParity].image;
        giBarriers[0].subresourceRange = giRange;
        giBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        giBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        giBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        giBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[1].image = *perImage[writeParity].image;
        giBarriers[1].subresourceRange = giRange;
        giBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, giBarriers);
    }

    if (historyIndex < m_restirGiMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange giMetaRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> giMetaBarriers{};
        giMetaBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[0].image = *perImage[readParity].image;
        giMetaBarriers[0].subresourceRange = giMetaRange;
        giMetaBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giMetaBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        giMetaBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[1].image = *perImage[writeParity].image;
        giMetaBarriers[1].subresourceRange = giMetaRange;
        giMetaBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giMetaBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, giMetaBarriers);
    }

    if (historyIndex < m_restirGiSpatialPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiSpatialPerImage[historyIndex];

        const vk::ImageSubresourceRange spatialRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> spatialBarriers{};
        spatialBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[0].image = *perImage[readParity].image;
        spatialBarriers[0].subresourceRange = spatialRange;
        spatialBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        spatialBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[1].image = *perImage[writeParity].image;
        spatialBarriers[1].subresourceRange = spatialRange;
        spatialBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            {}, {}, {}, spatialBarriers);
    }

    if (historyIndex < m_restirGiSpatialMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiSpatialMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange spatialMetaRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                                         1);
        std::array<vk::ImageMemoryBarrier, 2> spatialMetaBarriers{};
        spatialMetaBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[0].image = *perImage[readParity].image;
        spatialMetaBarriers[0].subresourceRange = spatialMetaRange;
        spatialMetaBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialMetaBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        spatialMetaBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[1].image = *perImage[writeParity].image;
        spatialMetaBarriers[1].subresourceRange = spatialMetaRange;
        spatialMetaBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialMetaBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            {}, {}, {}, spatialMetaBarriers);
    }

    if (!m_nrdPerImage.empty()) {
        const vk::ImageSubresourceRange nrdRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier nrdHistoryBarrier{};
        nrdHistoryBarrier.oldLayout = vk::ImageLayout::eGeneral;
        nrdHistoryBarrier.newLayout = vk::ImageLayout::eGeneral;
        nrdHistoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        nrdHistoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        nrdHistoryBarrier.image = *m_nrdPerImage[0].diffOut.image;
        nrdHistoryBarrier.subresourceRange = nrdRange;
        nrdHistoryBarrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        nrdHistoryBarrier.dstAccessMask =
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, nrdHistoryBarrier);
    }

    std::array<vk::ClearValue, 2> clearValues{};
    clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f});
    clearValues[1].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

    vk::RenderPassBeginInfo renderPassInfo{};
    renderPassInfo.renderPass = *m_renderPass.get();
    renderPassInfo.framebuffer = *m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = vk::Offset2D{0, 0};
    renderPassInfo.renderArea.extent = m_context.getSwapchainExtent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    m_commandBuffers[imageIndex].beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

    m_chunkPipeline.bind(m_commandBuffers[imageIndex]);
    m_chunkPipeline.pushViewProjection(m_commandBuffers[imageIndex], viewProjection);

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    m_commandBuffers[imageIndex].setViewport(0, viewport);

    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{0, 0};
    scissor.extent = extent;
    m_commandBuffers[imageIndex].setScissor(0, scissor);

    const vk::DescriptorSet modelDescriptorSet = *m_modelDescriptorSets[imageIndex];
    const vk::DescriptorSet giDescriptorSet =
        (imageIndex < m_giDescriptorSets.size()) ? *m_giDescriptorSets[imageIndex] : VK_NULL_HANDLE;
    const auto chunkCpuStart = std::chrono::steady_clock::now();

    if (!frameData.indirectCommands.empty() && imageIndex < m_perImageDrawResources.size()) {
        const PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
        if (resources.indirectCommandBuffer != nullptr) {
            const vk::DeviceSize stride = sizeof(IndexedIndirectCommand);
            const bool supportsMultiDrawIndirect = m_context.isMultiDrawIndirectEnabled();
            const bool supportsIndirectFirstInstance =
                m_context.isDrawIndirectFirstInstanceEnabled();
            for (const RenderIndirectBatch &batch : frameData.indirectBatches) {
                if (!batch.mesh || batch.commandCount == 0 ||
                    batch.firstCommand >= frameData.indirectCommands.size()) {
                    continue;
                }

                const uint32_t maxCommandCount =
                    static_cast<uint32_t>(frameData.indirectCommands.size() - batch.firstCommand);
                const uint32_t clampedCommandCount = std::min(batch.commandCount, maxCommandCount);
                if (clampedCommandCount == 0) {
                    continue;
                }

                batch.mesh->bind(m_commandBuffers[imageIndex]);

                vk::DescriptorSet textureDescriptorSet = m_fallbackArrayTexture.getDescriptorSet();
                if (batch.texture && batch.texture->getDescriptorSet() != VK_NULL_HANDLE) {
                    textureDescriptorSet = batch.texture->getDescriptorSet();
                }

                const std::array<vk::DescriptorSet, 3> descriptorSets = {
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet};
                m_commandBuffers[imageIndex].bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                                                *m_chunkPipeline.getLayout(), 0,
                                                                descriptorSets, {});

                if (!supportsMultiDrawIndirect && clampedCommandCount > 1) {
                    for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                        const IndexedIndirectCommand &command =
                            frameData.indirectCommands[batch.firstCommand + i];
                        if (command.indexCount == 0 || command.instanceCount == 0) {
                            continue;
                        }

                        if (!supportsIndirectFirstInstance && command.firstInstance != 0) {
                            m_commandBuffers[imageIndex].drawIndexed(
                                command.indexCount, command.instanceCount, command.firstIndex,
                                command.vertexOffset, command.firstInstance);
                            continue;
                        }

                        const vk::DeviceSize offset =
                            static_cast<vk::DeviceSize>(batch.firstCommand + i) * stride;
                        m_commandBuffers[imageIndex].drawIndexedIndirect(
                            *resources.indirectCommandBuffer, offset, 1,
                            static_cast<uint32_t>(stride));
                    }
                    continue;
                }

                bool requiresNonZeroFirstInstance = false;
                if (!supportsIndirectFirstInstance) {
                    for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                        const IndexedIndirectCommand &command =
                            frameData.indirectCommands[batch.firstCommand + i];
                        if (command.firstInstance != 0) {
                            requiresNonZeroFirstInstance = true;
                            break;
                        }
                    }
                }

                if (!requiresNonZeroFirstInstance) {
                    const vk::DeviceSize offset =
                        static_cast<vk::DeviceSize>(batch.firstCommand) * stride;
                    m_commandBuffers[imageIndex].drawIndexedIndirect(
                        *resources.indirectCommandBuffer, offset, clampedCommandCount,
                        static_cast<uint32_t>(stride));
                    continue;
                }

                for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                    const IndexedIndirectCommand &command =
                        frameData.indirectCommands[batch.firstCommand + i];
                    if (command.indexCount == 0 || command.instanceCount == 0) {
                        continue;
                    }
                    m_commandBuffers[imageIndex].drawIndexed(
                        command.indexCount, command.instanceCount, command.firstIndex,
                        command.vertexOffset, command.firstInstance);
                }
            }
        }
    }
    const auto chunkCpuEnd = std::chrono::steady_clock::now();
    outChunkCpuMs = measureMs(chunkCpuStart, chunkCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 1);
    }

    const auto modelCpuStart = std::chrono::steady_clock::now();
    if (!frameData.objects.empty()) {
        m_modelPipeline.bind(m_commandBuffers[imageIndex]);
        m_modelPipeline.pushViewProjection(m_commandBuffers[imageIndex], viewProjection);

        const uint32_t modelBaseInstance = static_cast<uint32_t>(frameData.modelMatrices.size());
        uint32_t objectIndex = 0;
        for (const RenderObject &object : frameData.objects) {
            const uint32_t firstInstance = modelBaseInstance + objectIndex;
            ++objectIndex;

            if (object.model == nullptr) {
                continue;
            }

            const std::vector<VkMesh> &meshes = object.model->getMeshes();
            if (meshes.empty()) {
                continue;
            }

            for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
                const VkMesh &mesh = meshes[meshIndex];
                if (mesh.getIndexCount() == 0) {
                    continue;
                }

                mesh.bind(m_commandBuffers[imageIndex]);

                vk::DescriptorSet textureDescriptorSet = m_fallback2DTexture.getDescriptorSet();
                if (object.meshTextures != nullptr && meshIndex < object.meshTextures->size()) {
                    const VkTexture *texture = (*object.meshTextures)[meshIndex];
                    if (texture != nullptr && texture->getDescriptorSet() != VK_NULL_HANDLE) {
                        textureDescriptorSet = texture->getDescriptorSet();
                    }
                }

                const std::array<vk::DescriptorSet, 3> descriptorSets = {
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet};
                m_commandBuffers[imageIndex].bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                                                *m_modelPipeline.getLayout(), 0,
                                                                descriptorSets, {});
                m_commandBuffers[imageIndex].drawIndexed(mesh.getIndexCount(), 1, 0, 0,
                                                         firstInstance);
            }
        }
    }
    const auto modelCpuEnd = std::chrono::steady_clock::now();
    outModelCpuMs = measureMs(modelCpuStart, modelCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 2);
    }

    const auto uiCpuStart = std::chrono::steady_clock::now();
#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    ImDrawData *drawData = frameData.uiDrawData;
    if (drawData != nullptr && drawData->CmdListsCount > 0 && ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO &io = ImGui::GetIO();
        if (io.BackendRendererUserData != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(drawData, *m_commandBuffers[imageIndex]);
        }
    }
#endif
    const auto uiCpuEnd = std::chrono::steady_clock::now();
    outUiCpuMs = measureMs(uiCpuStart, uiCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 3);
    }

    m_commandBuffers[imageIndex].endRenderPass();
    barrierNrdSignalsForCompute(imageIndex);
#if VOXELOPS_NRD_HEADERS
    dispatchNrdPass(imageIndex, frameData);
#endif
    dispatchRestirGiSpatialPass(imageIndex, frameData);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 4);
    }
    m_commandBuffers[imageIndex].end();
}
