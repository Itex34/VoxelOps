#include "VulkanRenderDevice.hpp"

#include "Camera.hpp"
#include "Vulkan/VulkanFrameBuilder.hpp"
#include "Vulkan/renderer/VulkanRenderer.hpp"
#include "Vulkan/vulkan/VulkanContext.hpp"
#include "data/GameData.hpp"
#include "../player/Player.hpp"
#include "../ui/debug/DebugUi.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>

VulkanRenderDevice::VulkanRenderDevice() = default;

VulkanRenderDevice::~VulkanRenderDevice() {
    shutdown();
}

bool VulkanRenderDevice::initialize(SDL_Window *window) {
    if (window == nullptr) {
        std::cerr << "[Vulkan] initialize failed: null SDL window.\n";
        return false;
    }

    try {
        m_window = window;

        m_context = std::make_unique<VulkanContext>();
        m_context->init(m_window);

        m_renderer = std::make_unique<VulkanRenderer>(*m_context);
        m_renderer->init();

        m_uploadContext.init(m_context->getDevice(), m_context->getGraphicsQueueFamily(),
                             m_context->getGraphicsQueue());

        if (!m_sceneUploader.initialize(*m_context, m_uploadContext)) {
            return false;
        }

        m_uploadContext.waitIdle();
        m_rtScene.initialize(*m_context, m_uploadContext, m_frameCounter);
        m_initialized = true;
        m_warnedUninitializedRender = false;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] initialize failed: " << e.what() << "\n";
        shutdown();
        return false;
    }
}

void VulkanRenderDevice::onWindowResized(int width, int height) {
    if (!m_initialized || !m_renderer) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    try {
        m_renderer->handleWindowResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] handleWindowResize failed: " << e.what() << "\n";
    }
}

bool VulkanRenderDevice::initializeDebugUi(DebugUi &debugUi, SDL_Window *window,
                                           void *nativeContext) {
    (void)nativeContext;
    if (!m_context || !m_renderer) {
        return false;
    }

    UiVulkanInitInfo initInfo{};
    initInfo.instance = getVkInstanceHandle();
    initInfo.physicalDevice = getVkPhysicalDeviceHandle();
    initInfo.device = getVkDeviceHandle();
    initInfo.queueFamily = getVkGraphicsQueueFamily();
    initInfo.queue = getVkGraphicsQueueHandle();
    initInfo.renderPass = getVkRenderPassHandle();
    initInfo.imageCount = getVkSwapchainImageCount();
    initInfo.minImageCount = 2;
    return debugUi.initializeForVulkan(window, initInfo);
}

void VulkanRenderDevice::appendBackendDebugUiFrameData(UiFrameData &frameData) const {
    const TimingSnapshot &vkTimings = getLastTimingSnapshot();
    frameData.vulkanTimingValid = true;
    frameData.vkCpuCommandRecordMs = vkTimings.cpuCommandRecordMs;
    frameData.vkCpuChunkPassMs = vkTimings.cpuChunkPassMs;
    frameData.vkCpuModelPassMs = vkTimings.cpuModelPassMs;
    frameData.vkCpuUiPassMs = vkTimings.cpuUiPassMs;
    frameData.vkCpuMeshSyncMs = vkTimings.cpuMeshSyncMs;
    frameData.vkCpuFrameBuildMs = vkTimings.cpuFrameBuildMs;
    frameData.vkCpuGiIntegrateMs = vkTimings.cpuGiIntegrateMs;
    frameData.vkGiTraceGridsUpdated = vkTimings.giTraceGridsUpdated;
    frameData.vkGiRaysCast = vkTimings.giRaysCast;
    frameData.vkGiAverageIrradianceLuma = vkTimings.giAverageIrradianceLuma;
    frameData.vkGpuTimingValid = vkTimings.gpuValid;
    frameData.vkGpuFrameMs = vkTimings.gpuFrameMs;
    frameData.vkGpuChunkPassMs = vkTimings.gpuChunkPassMs;
    frameData.vkGpuModelPassMs = vkTimings.gpuModelPassMs;
    frameData.vkGpuUiPassMs = vkTimings.gpuUiPassMs;
    frameData.vkGpuGiIntegrateMs = vkTimings.gpuGiIntegrateMs;
    frameData.vkGiHardwareRtSupported = vkTimings.giHardwareRtSupported;
    frameData.vkGiRtSceneReady = vkTimings.giRtSceneReady;
    frameData.vkGiTracingBackend =
        (vkTimings.giTracingBackend == GiTracingBackend::HardwareRt) ? 1 : 0;
    frameData.vkNrdBootstrapActive = vkTimings.nrdBootstrapActive;
    frameData.vkNrdBootstrapDispatchCount = vkTimings.nrdBootstrapDispatchCount;
}

VkInstance VulkanRenderDevice::getVkInstanceHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getInstance();
}

VkPhysicalDevice VulkanRenderDevice::getVkPhysicalDeviceHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getPhysicalDevice();
}

VkDevice VulkanRenderDevice::getVkDeviceHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getDevice();
}

uint32_t VulkanRenderDevice::getVkGraphicsQueueFamily() const noexcept {
    if (!m_context) {
        return 0;
    }
    return m_context->getGraphicsQueueFamily();
}

VkQueue VulkanRenderDevice::getVkGraphicsQueueHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getGraphicsQueue();
}

VkRenderPass VulkanRenderDevice::getVkRenderPassHandle() const noexcept {
    if (!m_renderer) {
        return VK_NULL_HANDLE;
    }
    return m_renderer->getRenderPassHandle();
}

uint32_t VulkanRenderDevice::getVkSwapchainImageCount() const noexcept {
    if (!m_renderer) {
        return 0;
    }
    return m_renderer->getSwapchainImageCount();
}

RenderDeviceCapabilities VulkanRenderDevice::getCapabilities() const noexcept {
    bool mdiUsable = false;
    if (!m_context) {
        mdiUsable = false;
    } else {
        mdiUsable = m_context->isMultiDrawIndirectEnabled() &&
                    m_context->isDrawIndirectFirstInstanceEnabled();
    }

    return RenderDeviceCapabilities{
        .api = RenderApi::Vulkan,
        .apiName = "Vulkan",
        .backendTier = GraphicsBackend::Performance,
        .backendName = "Vulkan",
        .mdiUsable = mdiUsable,
        .supportsGL43Shaders = false,
        .supportsBakedChunkLighting = false,
        .supportsGiRuntimeControls = true,
        .supportsFirstPersonViewmodel = false,
        .compositesUiInRenderFrame = true,
        .requiresOpenGlStateSetup = false};
}

void VulkanRenderDevice::renderFrame(RenderFrameParams &params) {
    const Camera &activeCamera = params.activeCamera;
    const Camera &cullingCamera = params.cullingCamera ? *params.cullingCamera : activeCamera;

    ChunkManager &chunkManager = params.chunkManager;
    const Player &player = params.player;
    ImDrawData *uiDrawData = params.uiDrawData;
    const glm::vec3 &sunDirection = params.sunDirection;

    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };

    if (!ensureInitialized()) {
        return;
    }

    const int width = std::max(0, GameData::screenWidth);
    const int height = std::max(0, GameData::screenHeight);
    if (width == 0 || height == 0) {
        return;
    }

    ++m_frameCounter;
    m_sceneUploader.collectRetiredChunkMeshes(m_frameCounter);
    m_rtScene.collectRetiredResources(m_frameCounter);

    const glm::ivec3 cullingChunk = chunkManager.worldToChunkPos(
        glm::ivec3(static_cast<int>(std::floor(cullingCamera.position.x)),
                   static_cast<int>(std::floor(cullingCamera.position.y)),
                   static_cast<int>(std::floor(cullingCamera.position.z))));

    const auto meshSyncStart = std::chrono::steady_clock::now();
    m_uploadContext.poll();
    if (m_context) {
        m_sceneUploader.syncChunkCache(chunkManager, cullingChunk, m_frameCounter, *m_context,
                                       m_uploadContext, m_rtScene);
    }
    if (m_context && m_rtScene.isDirty()) {
        (void)m_rtScene.rebuild(*m_context, m_frameCounter);
    }
    const auto meshSyncEnd = std::chrono::steady_clock::now();
    m_lastTimingSnapshot.cpuMeshSyncMs = measureMs(meshSyncStart, meshSyncEnd);
    m_lastTimingSnapshot.cpuGiIntegrateMs = 0.0f;
    m_lastTimingSnapshot.gpuGiIntegrateMs = 0.0f;
    m_lastTimingSnapshot.giTraceGridsUpdated = 0;
    m_lastTimingSnapshot.giRaysCast = 0;
    m_lastTimingSnapshot.giAverageIrradianceLuma = 0.0f;

    if (m_sceneUploader.empty() && !m_warnedNoCpuChunkMeshes) {
        std::cerr << "[Vulkan] No chunk CPU meshes available yet. Waiting for chunk mesher.\n";
        m_warnedNoCpuChunkMeshes = true;
    }
    if (!m_sceneUploader.empty()) {
        m_warnedNoCpuChunkMeshes = false;
    }

    const VkAccelerationStructureKHR sceneTlas = m_rtScene.activeTlas();
    const bool hardwareRtSupported =
        (m_context != nullptr) && m_context->isHardwareRayTracingSupported();
    const VulkanGiSettings::FrameDecision giDecision = m_giSettings.beginFrame(
        cullingCamera.position, sunDirection, hardwareRtSupported, sceneTlas);

    m_frameData.clear();
    m_frameData.giLighting.sceneTlas = sceneTlas;
    m_giSettings.fillLightingData(
        m_frameData.giLighting, giDecision,
        static_cast<uint32_t>(std::clamp(GameData::giNrdDebugView, 0, 5)));
    m_lastTimingSnapshot.giHardwareRtSupported = giDecision.hardwareRtSupported;
    m_lastTimingSnapshot.giRtSceneReady = giDecision.rtSceneReady;
    m_lastTimingSnapshot.giTracingBackend = giDecision.backend;

    (void)m_sceneUploader.ensureRemotePlayerAssetsLoaded(*m_context, m_uploadContext);
    const VulkanFrameBuildResult frameBuild = VulkanFrameBuilder::buildFrameData(
        m_frameData, m_sceneUploader.chunkRenderCache(), m_sceneUploader.atlasTexture(),
        activeCamera, cullingCamera, player, uiDrawData, width, height,
        m_sceneUploader.remotePlayerModel(), m_sceneUploader.remotePlayerTextureViews(),
        cullingChunk);

    const glm::mat4 view = frameBuild.view;
    const glm::mat4 projection = frameBuild.projection;
    const glm::mat4 viewProjection = frameBuild.viewProjection;
    m_lastTimingSnapshot.cpuFrameBuildMs = frameBuild.cpuFrameBuildMs;

    try {
        m_renderer->renderFrame(static_cast<uint32_t>(width), static_cast<uint32_t>(height), view,
                                projection, viewProjection, m_frameData);
        const VulkanRenderer::FrameTimingStats &rendererStats =
            m_renderer->getLastFrameTimingStats();
        m_lastTimingSnapshot.gpuValid = rendererStats.gpuValid;
        m_lastTimingSnapshot.gpuFrameMs = rendererStats.gpuFrameMs;
        m_lastTimingSnapshot.gpuChunkPassMs = rendererStats.gpuChunkPassMs;
        m_lastTimingSnapshot.gpuModelPassMs = rendererStats.gpuModelPassMs;
        m_lastTimingSnapshot.gpuUiPassMs = rendererStats.gpuUiPassMs;
        m_lastTimingSnapshot.cpuCommandRecordMs = rendererStats.cpuCommandRecordMs;
        m_lastTimingSnapshot.cpuChunkPassMs = rendererStats.cpuChunkPassMs;
        m_lastTimingSnapshot.cpuModelPassMs = rendererStats.cpuModelPassMs;
        m_lastTimingSnapshot.cpuUiPassMs = rendererStats.cpuUiPassMs;
        m_lastTimingSnapshot.nrdBootstrapActive = m_renderer->isNrdBootstrapActive();
        m_lastTimingSnapshot.nrdBootstrapDispatchCount = m_renderer->getNrdBootstrapDispatchCount();
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] renderFrame failed: " << e.what() << "\n";
    }
}


void VulkanRenderDevice::shutdown() {
    if (!m_initialized && !m_context && !m_renderer) {
        return;
    }

    try {
        if (m_context) {
            m_context->getDevice().waitIdle();
        }
    } catch (...) {
    }

    m_sceneUploader.shutdown();
    m_rtScene.reset();
    m_giSettings.reset();

    m_uploadContext.cleanup();

    if (m_renderer) {
        m_renderer->cleanup();
        m_renderer.reset();
    }
    if (m_context) {
        m_context->cleanup();
        m_context.reset();
    }

    m_window = nullptr;
    m_initialized = false;
}

bool VulkanRenderDevice::ensureInitialized() {
    if (m_initialized && m_context && m_renderer) {
        return true;
    }
    if (!m_warnedUninitializedRender) {
        std::cerr << "[Vulkan] render skipped: device not initialized.\n";
        m_warnedUninitializedRender = true;
    }
    return false;
}
