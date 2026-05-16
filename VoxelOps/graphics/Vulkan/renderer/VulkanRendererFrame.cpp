#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {
    bool parseBoolEnv(const char *name, bool defaultValue) {
        const char *env = std::getenv(name);
        if (env == nullptr) {
            return defaultValue;
        }
        if (env[0] == '1' || env[0] == 't' || env[0] == 'T' || env[0] == 'y' || env[0] == 'Y') {
            return true;
        }
        if (env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N') {
            return false;
        }
        return defaultValue;
    }

    uint32_t parseUintEnv(const char *name, uint32_t defaultValue) {
        const char *env = std::getenv(name);
        if (env == nullptr || env[0] == '\0') {
            return defaultValue;
        }
        char *end = nullptr;
        const unsigned long value = std::strtoul(env, &end, 10);
        if (end == env) {
            return defaultValue;
        }
        return static_cast<uint32_t>(std::max<unsigned long>(1ul, value));
    }

    bool timingLogsEnabled() {
        static const bool enabled = parseBoolEnv("VOXELOPS_VK_TIMING_LOG", false);
        return enabled;
    }

    uint32_t timingLogInterval() {
        static const uint32_t interval = parseUintEnv("VOXELOPS_VK_TIMING_LOG_INTERVAL", 60u);
        return interval;
    }

    float elapsedMs(
        const std::chrono::steady_clock::time_point &start,
        const std::chrono::steady_clock::time_point &end
    ) {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    }
} // namespace

void VulkanRenderer::renderFrame(
    uint32_t windowWidth,
    uint32_t windowHeight,
    const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    const glm::mat4 &viewProjection,
    const FrameRenderData &frameData
) {
    if (!m_initialized || m_framebuffers.empty()) {
        return;
    }
    const auto frameStart = std::chrono::steady_clock::now();
    const uint32_t frameLogId = m_frameCounterLow;
    float waitFrameFenceMs = 0.0f;
    float acquireMs = 0.0f;
    float waitImageFenceMs = 0.0f;
    float updateDrawBuffersMs = 0.0f;
    float updateGiDescMs = 0.0f;
    float nrdUpdateMs = 0.0f;
    float submitMs = 0.0f;
    float presentMs = 0.0f;
    const uint32_t chunkBatchCount = static_cast<uint32_t>(
        frameData.indirectBatches.empty() ? frameData.indirectCommands.size()
                                          : frameData.indirectBatches.size()
    );
    const uint32_t chunkCommandCount = static_cast<uint32_t>(frameData.indirectCommands.size());
    const uint32_t objectCount = static_cast<uint32_t>(frameData.objects.size());
    const uint32_t modelMatrixCount = static_cast<uint32_t>(frameData.modelMatrices.size());
    uint32_t chunkInstanceCount = 0;
    uint64_t chunkIndexCountTotal = 0;
    uint64_t chunkIndexInstanceCountTotal = 0;
    uint64_t chunkTriangleCountTotal = 0;
    for (const IndexedIndirectCommand &command : frameData.indirectCommands) {
        const uint64_t next = static_cast<uint64_t>(chunkInstanceCount) +
                              static_cast<uint64_t>(command.instanceCount);
        chunkInstanceCount = static_cast<uint32_t>(
            std::min<uint64_t>(next, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        );
        chunkIndexCountTotal += static_cast<uint64_t>(command.indexCount);
        chunkIndexInstanceCountTotal +=
            static_cast<uint64_t>(command.indexCount) * static_cast<uint64_t>(command.instanceCount);
        chunkTriangleCountTotal += (static_cast<uint64_t>(command.indexCount) / 3ull) *
                                   static_cast<uint64_t>(command.instanceCount);
    }
    const uint32_t uiCmdLists =
        (frameData.uiDrawData != nullptr) ? static_cast<uint32_t>(frameData.uiDrawData->CmdListsCount)
                                          : 0u;

    const vk::Extent2D currentExtent = m_context.getSwapchainExtent();
    if (currentExtent.width == 0 || currentExtent.height == 0) {
        handleWindowResize(windowWidth, windowHeight);
        return;
    }

    m_uploadContext.poll();

    const vk::raii::Device &device = m_context.getDevice();

    const vk::Fence currentFrameFence = m_frameSync.getCurrentFrameFence();
    std::array<vk::Fence, 1> currentFrameFenceArray = {currentFrameFence};
    const auto waitFrameFenceStart = std::chrono::steady_clock::now();
    (void)device.waitForFences(
        currentFrameFenceArray, vk::True, std::numeric_limits<uint64_t>::max()
    );
    const auto waitFrameFenceEnd = std::chrono::steady_clock::now();
    waitFrameFenceMs = elapsedMs(waitFrameFenceStart, waitFrameFenceEnd);
    uint32_t imageIndex = 0;
    vk::Result acquireResult = vk::Result::eSuccess;
    const auto acquireStart = std::chrono::steady_clock::now();
    try {
        auto acquire = m_context.getSwapchain().acquireNextImage(
            std::numeric_limits<uint64_t>::max(),
            m_frameSync.getCurrentImageAvailableSemaphore(),
            nullptr
        );
        acquireResult = acquire.result;
        imageIndex = acquire.value;
    } catch (const vk::OutOfDateKHRError &) {
        handleWindowResize(windowWidth, windowHeight);
        return;
    }
    const auto acquireEnd = std::chrono::steady_clock::now();
    acquireMs = elapsedMs(acquireStart, acquireEnd);

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
        const auto waitImageFenceStart = std::chrono::steady_clock::now();
        std::array<vk::Fence, 1> imageFenceArray = {imageFence};
        (void)device.waitForFences(imageFenceArray, vk::True, std::numeric_limits<uint64_t>::max());
        const auto waitImageFenceEnd = std::chrono::steady_clock::now();
        waitImageFenceMs = elapsedMs(waitImageFenceStart, waitImageFenceEnd);
        updateGpuTimingStatsForImage(imageIndex);
    } else {
        m_lastFrameTimingStats.gpuValid = false;
    }

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

    const auto updateDrawStart = std::chrono::steady_clock::now();
    updatePerImageDrawBuffers(
        imageIndex,
        combinedModelMatrices,
        frameData.indirectCommands,
        frameData.chunkSuperbatchVertices,
        frameData.chunkSuperbatchIndices
    );
    const auto updateDrawEnd = std::chrono::steady_clock::now();
    updateDrawBuffersMs = elapsedMs(updateDrawStart, updateDrawEnd);
    const auto updateGiStart = std::chrono::steady_clock::now();
    updateGiDescriptorSet(imageIndex, frameData, viewProjection);
    const auto updateGiEnd = std::chrono::steady_clock::now();
    updateGiDescMs = elapsedMs(updateGiStart, updateGiEnd);
    if (m_nrdBootstrap != nullptr) {
        const auto nrdUpdateStart = std::chrono::steady_clock::now();
        glm::mat4 prevViewMatrix = viewMatrix;
        glm::mat4 prevProjectionMatrix = projectionMatrix;
        bool hasPrevMatrices = frameData.giLighting.enabled && !frameData.giLighting.resetHistory &&
                               m_nrdPrevMatricesValid;
        if (hasPrevMatrices) {
            prevViewMatrix = m_nrdPrevView;
            prevProjectionMatrix = m_nrdPrevProjection;
        }
        m_nrdBootstrap->updateFrame(
            viewMatrix,
            projectionMatrix,
            prevViewMatrix,
            prevProjectionMatrix,
            hasPrevMatrices,
            frameData,
            currentExtent.width,
            currentExtent.height
        );
        const auto nrdUpdateEnd = std::chrono::steady_clock::now();
        nrdUpdateMs = elapsedMs(nrdUpdateStart, nrdUpdateEnd);
    }

    const uint32_t nrdDebugView = frameData.giLighting.nrdDebugView;
    const bool compositeDebugView =
        (nrdDebugView == 10u) || (nrdDebugView == 15u) || (nrdDebugView == 16u) ||
        ((nrdDebugView >= 30u) && (nrdDebugView <= 34u));
    const bool useNrdComposite = frameData.giLighting.pathTracingEnabled &&
                                 (nrdDebugView == 0u || compositeDebugView) &&
                                 m_nrdBootstrap != nullptr && m_nrdBootstrap->isActive() &&
                                 m_nrdCompositePipeline != nullptr &&
                                 imageIndex < m_compositeFramebuffers.size() &&
                                 imageIndex < m_giDescriptorSets.size();
    bool useAsyncNrdSubmit =
        m_nrdAsyncComputeEnabled && imageIndex < m_compositeCommandBuffers.size() &&
        imageIndex < m_nrdGraphicsToComputeSemaphores.size() &&
        imageIndex < m_nrdComputeToGraphicsSemaphores.size();
#if VOXELOPS_NRD_HEADERS
    useAsyncNrdSubmit = useAsyncNrdSubmit && imageIndex < m_nrdComputeCommandBuffers.size();
#endif
#if !VOXELOPS_NRD_HEADERS
    useAsyncNrdSubmit = false;
#endif

    m_commandBuffers[imageIndex].reset();
    if (useAsyncNrdSubmit) {
        m_compositeCommandBuffers[imageIndex].reset();
    }
#if VOXELOPS_NRD_HEADERS
    if (useAsyncNrdSubmit && imageIndex < m_nrdComputeCommandBuffers.size()) {
        m_nrdComputeCommandBuffers[imageIndex].reset();
    }
#endif
    float chunkCpuMs = 0.0f;
    float modelCpuMs = 0.0f;
    float uiCpuMs = 0.0f;
    const auto cpuRecordStart = std::chrono::steady_clock::now();
    recordCommandBuffer(
        imageIndex,
        viewProjection,
        frameData,
        chunkCpuMs,
        modelCpuMs,
        uiCpuMs,
        !useAsyncNrdSubmit
    );
    if (useAsyncNrdSubmit) {
#if VOXELOPS_NRD_HEADERS
        if (imageIndex < m_nrdComputeCommandBuffers.size()) {
            recordNrdComputeCommandBuffer(imageIndex, frameData);
        }
#endif
        recordCompositeCommandBuffer(imageIndex, frameData, useNrdComposite);
    }
    const auto cpuRecordEnd = std::chrono::steady_clock::now();
    m_lastFrameTimingStats.cpuCommandRecordMs = static_cast<float>(
        std::chrono::duration<double, std::milli>(cpuRecordEnd - cpuRecordStart).count()
    );
    m_lastFrameTimingStats.cpuChunkPassMs = chunkCpuMs;
    m_lastFrameTimingStats.cpuModelPassMs = modelCpuMs;
    m_lastFrameTimingStats.cpuUiPassMs = uiCpuMs;

    const vk::Semaphore waitSemaphore = m_frameSync.getCurrentImageAvailableSemaphore();
    const vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    const vk::CommandBuffer commandBuffer = *m_commandBuffers[imageIndex];
    const vk::Semaphore signalSemaphore = m_frameSync.getRenderFinishedSemaphore(imageIndex);

    (void)device.resetFences(currentFrameFenceArray);
    try {
        const auto submitStart = std::chrono::steady_clock::now();
        if (!useAsyncNrdSubmit) {
            vk::SubmitInfo submitInfo{};
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &waitSemaphore;
            submitInfo.pWaitDstStageMask = &waitStage;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &signalSemaphore;
            m_context.getGraphicsQueue().submit(submitInfo, currentFrameFence);
        } else {
            const vk::Semaphore graphicsToCompute = *m_nrdGraphicsToComputeSemaphores[imageIndex];
            const vk::Semaphore computeToGraphics = *m_nrdComputeToGraphicsSemaphores[imageIndex];

            vk::SubmitInfo graphicsMainSubmit{};
            graphicsMainSubmit.waitSemaphoreCount = 1;
            graphicsMainSubmit.pWaitSemaphores = &waitSemaphore;
            graphicsMainSubmit.pWaitDstStageMask = &waitStage;
            graphicsMainSubmit.commandBufferCount = 1;
            graphicsMainSubmit.pCommandBuffers = &commandBuffer;
            graphicsMainSubmit.signalSemaphoreCount = 1;
            graphicsMainSubmit.pSignalSemaphores = &graphicsToCompute;
            m_context.getGraphicsQueue().submit(graphicsMainSubmit, nullptr);

#if VOXELOPS_NRD_HEADERS
            if (imageIndex < m_nrdComputeCommandBuffers.size()) {
                const vk::CommandBuffer computeCommandBuffer = *m_nrdComputeCommandBuffers[imageIndex];
                const vk::PipelineStageFlags computeWaitStage =
                    vk::PipelineStageFlagBits::eComputeShader;
                vk::SubmitInfo computeSubmit{};
                computeSubmit.waitSemaphoreCount = 1;
                computeSubmit.pWaitSemaphores = &graphicsToCompute;
                computeSubmit.pWaitDstStageMask = &computeWaitStage;
                computeSubmit.commandBufferCount = 1;
                computeSubmit.pCommandBuffers = &computeCommandBuffer;
                computeSubmit.signalSemaphoreCount = 1;
                computeSubmit.pSignalSemaphores = &computeToGraphics;
                m_context.getRtBuildQueue().submit(computeSubmit, nullptr);
            } else
#endif
            {
                // Fallback: if compute command recording is unavailable, chain directly.
                const vk::PipelineStageFlags passthroughWaitStage =
                    vk::PipelineStageFlagBits::eColorAttachmentOutput;
                vk::SubmitInfo passthroughSubmit{};
                passthroughSubmit.waitSemaphoreCount = 1;
                passthroughSubmit.pWaitSemaphores = &graphicsToCompute;
                passthroughSubmit.pWaitDstStageMask = &passthroughWaitStage;
                passthroughSubmit.signalSemaphoreCount = 1;
                passthroughSubmit.pSignalSemaphores = &computeToGraphics;
                m_context.getGraphicsQueue().submit(passthroughSubmit, nullptr);
            }

            const vk::CommandBuffer compositeCommandBuffer = *m_compositeCommandBuffers[imageIndex];
            const vk::PipelineStageFlags compositeWaitStage =
                vk::PipelineStageFlagBits::eFragmentShader;
            vk::SubmitInfo graphicsCompositeSubmit{};
            graphicsCompositeSubmit.waitSemaphoreCount = 1;
            graphicsCompositeSubmit.pWaitSemaphores = &computeToGraphics;
            graphicsCompositeSubmit.pWaitDstStageMask = &compositeWaitStage;
            graphicsCompositeSubmit.commandBufferCount = 1;
            graphicsCompositeSubmit.pCommandBuffers = &compositeCommandBuffer;
            graphicsCompositeSubmit.signalSemaphoreCount = 1;
            graphicsCompositeSubmit.pSignalSemaphores = &signalSemaphore;
            m_context.getGraphicsQueue().submit(graphicsCompositeSubmit, currentFrameFence);
        }
        const auto submitEnd = std::chrono::steady_clock::now();
        submitMs = elapsedMs(submitStart, submitEnd);
        m_frameSync.setImageInFlightFence(imageIndex, currentFrameFence);
    } catch (...) {
        m_frameSync.setImageInFlightFence(imageIndex, imageFence);
        m_frameSync.recreateCurrentFrameFenceSignaled(device);
        throw;
    }
    updateGiHistoryAfterSubmit(
        frameData, viewMatrix, projectionMatrix, viewProjection, currentFrameFence
    );

    ++m_frameCounterLow;

    const vk::SwapchainKHR swapchain = *m_context.getSwapchain();
    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &signalSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    try {
        const auto presentStart = std::chrono::steady_clock::now();
        const vk::Result presentResult = m_context.getPresentQueue().presentKHR(presentInfo);
        const auto presentEnd = std::chrono::steady_clock::now();
        presentMs = elapsedMs(presentStart, presentEnd);
        if (m_context.shouldRecreateSwapchain(presentResult)) {
            handleWindowResize(windowWidth, windowHeight);
        }
    } catch (const vk::OutOfDateKHRError &) {
        handleWindowResize(windowWidth, windowHeight);
    }

    if (timingLogsEnabled()) {
        const uint32_t interval = timingLogInterval();
        if (interval > 0u && (frameLogId % interval) == 0u) {
            const auto frameEnd = std::chrono::steady_clock::now();
            const float frameMs = elapsedMs(frameStart, frameEnd);
            std::cout << "[Vulkan][Timing][Renderer] frame=" << frameLogId
                      << " image=" << imageIndex
                      << " totalCpuMs=" << frameMs
                      << " waitFrameFenceMs=" << waitFrameFenceMs
                      << " acquireMs=" << acquireMs
                      << " waitImageFenceMs=" << waitImageFenceMs
                      << " updateDrawBuffersMs=" << updateDrawBuffersMs
                      << " updateGiDescMs=" << updateGiDescMs
                      << " nrdUpdateMs=" << nrdUpdateMs
                      << " cmdRecordMs=" << m_lastFrameTimingStats.cpuCommandRecordMs
                      << " descriptorBinds=" << m_lastFrameTimingStats.descriptorBindCount
                      << " chunkDescriptorBinds="
                      << m_lastFrameTimingStats.chunkDescriptorBindCount
                      << " modelDescriptorBinds="
                      << m_lastFrameTimingStats.modelDescriptorBindCount
                      << " drawIndexedIndirectCalls="
                      << m_lastFrameTimingStats.drawIndexedIndirectCount
                      << " drawIndexedCalls=" << m_lastFrameTimingStats.drawIndexedCount
                      << " submitMs=" << submitMs
                      << " presentMs=" << presentMs
                      << " chunkBatches=" << chunkBatchCount
                      << " chunkCommands=" << chunkCommandCount
                      << " chunkInstances=" << chunkInstanceCount
                      << " chunkIndices=" << chunkIndexCountTotal
                      << " chunkIndexInstances=" << chunkIndexInstanceCountTotal
                      << " chunkTriangles=" << chunkTriangleCountTotal
                      << " chunkSuperbatch=" << (frameData.chunkSuperbatchEnabled ? 1 : 0)
                      << " sceneObjects=" << objectCount
                      << " modelMatrices=" << modelMatrixCount
                      << " uiCmdLists=" << uiCmdLists
                      << " gpuFrameMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuFrameMs
                                                           : -1.0f)
                      << " gpuMainMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuMainPassMs
                                                           : -1.0f)
                      << " gpuChunkMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuChunkPassMs
                                                           : -1.0f)
                      << " gpuMainSetupMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuMainSetupMs
                                                           : -1.0f)
                      << " gpuChunkDrawMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuChunkDrawMs
                                                           : -1.0f)
                      << " gpuModelMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuModelPassMs
                                                           : -1.0f)
                      << " gpuMainTailMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuMainTailMs
                                                           : -1.0f)
                      << " gpuNrdDispatchMs="
                      << (m_lastFrameTimingStats.gpuValid
                              ? m_lastFrameTimingStats.gpuNrdDispatchMs
                              : -1.0f)
                      << " gpuCompositeMs="
                      << (m_lastFrameTimingStats.gpuValid
                              ? m_lastFrameTimingStats.gpuCompositePassMs
                              : -1.0f)
                      << " gpuUiMs="
                      << (m_lastFrameTimingStats.gpuValid ? m_lastFrameTimingStats.gpuUiPassMs
                                                           : -1.0f)
                      << "\n";
        }
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
