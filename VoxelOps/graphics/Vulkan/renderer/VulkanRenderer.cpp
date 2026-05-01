#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"
#include "RestirConfig.hpp"

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


using namespace Vulkan::Restir;


namespace {
static_assert(sizeof(IndexedIndirectCommand) == sizeof(vk::DrawIndexedIndirectCommand));
static_assert(alignof(IndexedIndirectCommand) == alignof(vk::DrawIndexedIndirectCommand));
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
























