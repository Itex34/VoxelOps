#include "VulkanRenderDevice.hpp"

#include "Camera.hpp"
#include "Vulkan/VulkanFrameBuilder.hpp"
#include "Vulkan/renderer/VulkanRenderer.hpp"
#include "Vulkan/vulkan/VulkanContext.hpp"
#include "data/GameData.hpp"
#include "../ui/debug/DebugUi.hpp"
#include "../../Shared/world/Constants.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>

namespace {
    constexpr float kStreamingBudgetEmaAlpha = 0.20f;
    constexpr uint32_t kStreamingTierHoldFrames = 12u;
    constexpr uint8_t kStreamingTierVeryFast = 0u;
    constexpr uint8_t kStreamingTierFast = 1u;
    constexpr uint8_t kStreamingTierMedium = 2u;
    constexpr uint8_t kStreamingTierSlow = 3u;

    int floorDivLocal(int a, int b) {
        int q = a / b;
        int r = a % b;
        if ((r != 0) && ((r > 0) != (b > 0))) {
            q--;
        }
        return q;
    }

    uint8_t pickInitialStreamingTier(float frameMs) {
        if (frameMs < 9.5f) {
            return kStreamingTierVeryFast;
        }
        if (frameMs < 11.5f) {
            return kStreamingTierFast;
        }
        if (frameMs < 14.0f) {
            return kStreamingTierMedium;
        }
        return kStreamingTierSlow;
    }

    uint8_t updateStreamingTierWithHysteresis(uint8_t currentTier, float emaFrameMs) {
        switch (currentTier) {
        case kStreamingTierVeryFast:
            return (emaFrameMs > 10.5f) ? kStreamingTierFast : currentTier;
        case kStreamingTierFast:
            if (emaFrameMs < 8.8f) {
                return kStreamingTierVeryFast;
            }
            return (emaFrameMs > 12.5f) ? kStreamingTierMedium : currentTier;
        case kStreamingTierMedium:
            if (emaFrameMs < 10.8f) {
                return kStreamingTierFast;
            }
            return (emaFrameMs > 15.0f) ? kStreamingTierSlow : currentTier;
        case kStreamingTierSlow:
            return (emaFrameMs < 13.0f) ? kStreamingTierMedium : currentTier;
        }
        return currentTier;
    }

    float chunkUploadBudgetMsForTier(uint8_t tier) {
        switch (tier) {
        case kStreamingTierVeryFast:
            return 1.2f;
        case kStreamingTierFast:
            return 0.85f;
        case kStreamingTierMedium:
            return 0.55f;
        case kStreamingTierSlow:
            return 0.30f;
        }
        return 0.55f;
    }

    int rtActiveRadiusChunksForTier(uint8_t tier) {
        (void)tier;
        return 8;
    }

    uint32_t rtBuildPrimitiveBudgetForTier(uint8_t tier) {
        switch (tier) {
        case kStreamingTierVeryFast:
            return 22000u;
        case kStreamingTierFast:
            return 14000u;
        case kStreamingTierMedium:
            return 8000u;
        case kStreamingTierSlow:
            return 4000u;
        }
        return 8000u;
    }

    VkDeviceSize rtBuildByteBudgetForTier(uint8_t tier) {
        switch (tier) {
        case kStreamingTierVeryFast:
            return 2ull * 1024ull * 1024ull;
        case kStreamingTierFast:
            return 1280ull * 1024ull;
        case kStreamingTierMedium:
            return 768ull * 1024ull;
        case kStreamingTierSlow:
            return 512ull * 1024ull;
        }
        return 768ull * 1024ull;
    }

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

    bool giRebuildLogsEnabled() {
        static const bool enabled = parseBoolEnv("VOXELOPS_VK_GI_REBUILD_LOG", false);
        return enabled;
    }
} // namespace

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

        m_uploadContext.init(
            m_context->getDevice(),
            m_context->getVmaAllocator(),
            m_context->getGraphicsQueueFamily(),
            m_context->getGraphicsQueue()
        );

        if (!m_sceneUploader.initialize(*m_context, m_uploadContext)) {
            return false;
        }

        m_uploadContext.waitIdle();
        m_rtScene.initialize(*m_context, m_uploadContext, m_frameCounter);
        m_lastRtTlasBuildFrame = 0;
        m_lastStreamingSyncFrame = 0;
        m_lastGiRebuildKickFrame = 0;
        m_lastGiChunkCacheContentVersion = 0;
        m_recentGiIntegrateMs = 0.0f;
        m_lastCpuChunkMeshesVersion = 0;
        m_lastSyncCullingChunk = glm::ivec3(0);
        m_lastSyncCullingChunkValid = false;
        m_streamingBudgetFrameMsEma = 0.0f;
        m_streamingBudgetFrameMsEmaValid = false;
        m_streamingBudgetTier = kStreamingTierMedium;
        m_streamingBudgetTierHoldFrames = 0;
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

bool VulkanRenderDevice::initializeDebugUi(
    DebugUi &debugUi, SDL_Window *window, void *nativeContext
) {
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
        .supportsBakedChunkLighting = false,
        .supportsGiRuntimeControls = true,
        .supportsFirstPersonViewmodel = false,
        .compositesUiInRenderFrame = true
    };
}

void VulkanRenderDevice::renderFrame(RenderScene &scene) {
    const auto frameStart = std::chrono::steady_clock::now();
    const Camera &activeCamera = scene.activeCamera;
    const Camera &cullingCamera = scene.cullingCamera ? *scene.cullingCamera : activeCamera;
    ImDrawData *uiDrawData = scene.uiDrawData;
    const glm::vec3 &sunDirection = scene.sunDirection;
    if (scene.chunkWorld.cpuChunkMeshes == nullptr || scene.chunkWorld.chunks == nullptr) {
        return;
    }

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
    m_giSceneBuffers.collectRetiredBuffers(m_frameCounter);

    const glm::ivec3 cullingBlockPos(
        static_cast<int>(std::floor(cullingCamera.position.x)),
        static_cast<int>(std::floor(cullingCamera.position.y)),
        static_cast<int>(std::floor(cullingCamera.position.z))
    );
    const glm::ivec3 cullingChunk(
        floorDivLocal(cullingBlockPos.x, CHUNK_SIZE),
        floorDivLocal(cullingBlockPos.y, CHUNK_SIZE),
        floorDivLocal(cullingBlockPos.z, CHUNK_SIZE)
    );

    m_lastTimingSnapshot.cpuMeshSyncMs = 0.0f;
    m_lastTimingSnapshot.cpuGiIntegrateMs = 0.0f;
    m_lastTimingSnapshot.gpuGiIntegrateMs = 0.0f;
    m_lastTimingSnapshot.giTraceGridsUpdated = 0;
    m_lastTimingSnapshot.giRaysCast = 0;
    m_lastTimingSnapshot.giAverageIrradianceLuma = 0.0f;
    m_lastTimingSnapshot.descriptorBindCount = 0;
    m_lastTimingSnapshot.chunkDescriptorBindCount = 0;
    m_lastTimingSnapshot.modelDescriptorBindCount = 0;
    m_lastTimingSnapshot.drawIndexedIndirectCount = 0;
    m_lastTimingSnapshot.drawIndexedCount = 0;
    m_lastTimingSnapshot.chunkBatchCount = 0;
    m_lastTimingSnapshot.chunkCommandCount = 0;
    m_lastTimingSnapshot.chunkInstanceCount = 0;
    m_lastTimingSnapshot.chunkIndexCountTotal = 0;
    m_lastTimingSnapshot.chunkIndexInstanceCountTotal = 0;
    m_lastTimingSnapshot.chunkTriangleCountTotal = 0;
    m_lastTimingSnapshot.objectCount = 0;
    m_lastTimingSnapshot.modelMatrixCount = 0;

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
        cullingCamera.position,
        sunDirection,
        hardwareRtSupported,
        sceneTlas,
        std::clamp(GameData::giTracingBackendPreference, 0, 2)
    );

    m_frameData.clear();
    m_frameData.giLighting.sceneTlas = sceneTlas;
    m_giSettings.fillLightingData(
        m_frameData.giLighting,
        giDecision,
        static_cast<uint32_t>(std::clamp(GameData::giNrdDebugView, 0, 34)),
        static_cast<uint32_t>(std::clamp(GameData::giNrdGuideOverride, 0, 2))
    );
    m_lastTimingSnapshot.giHardwareRtSupported = giDecision.hardwareRtSupported;
    m_lastTimingSnapshot.giRtSceneReady = giDecision.rtSceneReady;
    m_lastTimingSnapshot.giTracingBackend = giDecision.backend;

    m_giSceneBuffers.applyToLighting(m_frameData.giLighting);
    if (m_giSceneBuffers.valid()) {
        m_lastTimingSnapshot.giTraceGridsUpdated =
            static_cast<uint32_t>(m_giSceneBuffers.chunkCount());
    }
    (void)m_sceneUploader.ensureRemotePlayerAssetsLoaded(*m_context, m_uploadContext);
    const VulkanFrameBuildResult frameBuild = VulkanFrameBuilder::buildFrameData(
        m_frameData,
        m_sceneUploader.chunkRenderCache(),
        m_sceneUploader.atlasTexture(),
        activeCamera,
        cullingCamera,
        scene.localPlayerPosition,
        scene.chunkRenderDistance,
        scene.remotePlayers,
        uiDrawData,
        scene.nativeUiDrawData,
        width,
        height,
        m_sceneUploader.remotePlayerModel(),
        m_sceneUploader.remotePlayerTextureViews(),
        cullingChunk
    );

    const glm::mat4 view = frameBuild.view;
    const glm::mat4 projection = frameBuild.projection;
    const glm::mat4 viewProjection = frameBuild.viewProjection;
    m_lastTimingSnapshot.cpuFrameBuildMs = frameBuild.cpuFrameBuildMs;
    m_lastTimingSnapshot.chunkBatchCount = static_cast<uint32_t>(
        m_frameData.indirectBatches.empty() ? m_frameData.indirectCommands.size()
                                            : m_frameData.indirectBatches.size()
    );
    m_lastTimingSnapshot.chunkCommandCount =
        static_cast<uint32_t>(m_frameData.indirectCommands.size());
    m_lastTimingSnapshot.objectCount = static_cast<uint32_t>(m_frameData.objects.size());
    m_lastTimingSnapshot.modelMatrixCount = static_cast<uint32_t>(m_frameData.modelMatrices.size());
    m_lastTimingSnapshot.chunkInstanceCount = 0;
    m_lastTimingSnapshot.chunkIndexCountTotal = 0;
    m_lastTimingSnapshot.chunkIndexInstanceCountTotal = 0;
    m_lastTimingSnapshot.chunkTriangleCountTotal = 0;
    for (const IndexedIndirectCommand &command : m_frameData.indirectCommands) {
        const uint64_t next = static_cast<uint64_t>(m_lastTimingSnapshot.chunkInstanceCount) +
                              static_cast<uint64_t>(command.instanceCount);
        m_lastTimingSnapshot.chunkInstanceCount = static_cast<uint32_t>(
            std::min<uint64_t>(next, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        );
        m_lastTimingSnapshot.chunkIndexCountTotal += static_cast<uint64_t>(command.indexCount);
        m_lastTimingSnapshot.chunkIndexInstanceCountTotal +=
            static_cast<uint64_t>(command.indexCount) * static_cast<uint64_t>(command.instanceCount);
        m_lastTimingSnapshot.chunkTriangleCountTotal +=
            (static_cast<uint64_t>(command.indexCount) / 3ull) *
            static_cast<uint64_t>(command.instanceCount);
    }

    float rendererFrameCpuMs = 0.0f;
    try {
        const auto rendererStart = std::chrono::steady_clock::now();
        m_renderer->renderFrame(
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            view,
            projection,
            viewProjection,
            m_frameData
        );
        const auto rendererEnd = std::chrono::steady_clock::now();
        rendererFrameCpuMs = measureMs(rendererStart, rendererEnd);
        const VulkanRenderer::FrameTimingStats &rendererStats =
            m_renderer->getLastFrameTimingStats();
        m_lastTimingSnapshot.gpuValid = rendererStats.gpuValid;
        m_lastTimingSnapshot.gpuFrameMs = rendererStats.gpuFrameMs;
        m_lastTimingSnapshot.gpuChunkPassMs = rendererStats.gpuChunkPassMs;
        m_lastTimingSnapshot.gpuMainSetupMs = rendererStats.gpuMainSetupMs;
        m_lastTimingSnapshot.gpuChunkDrawMs = rendererStats.gpuChunkDrawMs;
        m_lastTimingSnapshot.gpuModelPassMs = rendererStats.gpuModelPassMs;
        m_lastTimingSnapshot.gpuUiPassMs = rendererStats.gpuUiPassMs;
        m_lastTimingSnapshot.gpuMainTailMs = rendererStats.gpuMainTailMs;
        m_lastTimingSnapshot.gpuMainPassMs = rendererStats.gpuMainPassMs;
        m_lastTimingSnapshot.gpuNrdDispatchMs = rendererStats.gpuNrdDispatchMs;
        m_lastTimingSnapshot.gpuCompositePassMs = rendererStats.gpuCompositePassMs;
        m_lastTimingSnapshot.cpuCommandRecordMs = rendererStats.cpuCommandRecordMs;
        m_lastTimingSnapshot.cpuChunkPassMs = rendererStats.cpuChunkPassMs;
        m_lastTimingSnapshot.cpuModelPassMs = rendererStats.cpuModelPassMs;
        m_lastTimingSnapshot.cpuUiPassMs = rendererStats.cpuUiPassMs;
        m_lastTimingSnapshot.descriptorBindCount = rendererStats.descriptorBindCount;
        m_lastTimingSnapshot.chunkDescriptorBindCount = rendererStats.chunkDescriptorBindCount;
        m_lastTimingSnapshot.modelDescriptorBindCount = rendererStats.modelDescriptorBindCount;
        m_lastTimingSnapshot.drawIndexedIndirectCount = rendererStats.drawIndexedIndirectCount;
        m_lastTimingSnapshot.drawIndexedCount = rendererStats.drawIndexedCount;
        m_lastTimingSnapshot.nrdBootstrapActive = m_renderer->isNrdBootstrapActive();
        m_lastTimingSnapshot.nrdBootstrapDispatchCount = m_renderer->getNrdBootstrapDispatchCount();
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] renderFrame failed: " << e.what() << "\n";
    }

    const auto meshSyncStart = std::chrono::steady_clock::now();
    m_uploadContext.poll();
    const float frameMsSample = static_cast<float>(GameData::deltaTime * 1000.0);
    if (!m_streamingBudgetFrameMsEmaValid) {
        m_streamingBudgetFrameMsEma = frameMsSample;
        m_streamingBudgetFrameMsEmaValid = true;
        m_streamingBudgetTier = pickInitialStreamingTier(frameMsSample);
        m_streamingBudgetTierHoldFrames = kStreamingTierHoldFrames;
    } else {
        m_streamingBudgetFrameMsEma = ((1.0f - kStreamingBudgetEmaAlpha) * m_streamingBudgetFrameMsEma) +
                                      (kStreamingBudgetEmaAlpha * frameMsSample);
        if (m_streamingBudgetTierHoldFrames > 0) {
            --m_streamingBudgetTierHoldFrames;
        } else {
            const uint8_t nextTier = updateStreamingTierWithHysteresis(
                m_streamingBudgetTier, m_streamingBudgetFrameMsEma
            );
            if (nextTier != m_streamingBudgetTier) {
                m_streamingBudgetTier = nextTier;
                m_streamingBudgetTierHoldFrames = kStreamingTierHoldFrames;
            }
        }
    }

    const uint64_t cpuChunkMeshesVersion = scene.chunkWorld.cpuChunkMeshesVersion;
    const bool cpuChunkMeshesChanged = (cpuChunkMeshesVersion != m_lastCpuChunkMeshesVersion);
    const bool cullingChunkChanged =
        !m_lastSyncCullingChunkValid || (m_lastSyncCullingChunk != cullingChunk);
    const bool periodicSyncTick = ((m_frameCounter & 3u) == 0u);
    const int rtActiveRadiusChunks = rtActiveRadiusChunksForTier(m_streamingBudgetTier);
    const uint32_t rtBuildPrimitiveBudget = rtBuildPrimitiveBudgetForTier(m_streamingBudgetTier);
    const VkDeviceSize rtBuildByteBudget = rtBuildByteBudgetForTier(m_streamingBudgetTier);
    bool allowOversizedRtBuild = false;
    size_t rtBlasUploadsSubmitted = 0u;
    const char *rtTlasDecision = "no_context";

    if (m_context) {
        rtTlasDecision = "clean";
        m_rtScene.pollFinishedBlasBuilds(*m_context, m_frameCounter);
        m_rtScene.pollFinishedTlasBuild(*m_context, m_frameCounter);
        const bool highPriorityRtWork = m_rtScene.hasHighPriorityBuildWork();
        const bool rtPendingBuildWork = m_rtScene.hasPendingBuildWork();
        uint64_t streamingSyncIntervalFrames = 4u;
        if (highPriorityRtWork) {
            streamingSyncIntervalFrames = 1u;
        } else if (rtPendingBuildWork) {
            streamingSyncIntervalFrames =
                (m_streamingBudgetTier == kStreamingTierVeryFast) ? 2u : 4u;
        }
        const bool streamingSyncCadenceReached =
            (m_frameCounter >= (m_lastStreamingSyncFrame + streamingSyncIntervalFrames));
        const bool shouldSyncChunkCache = cpuChunkMeshesChanged || cullingChunkChanged ||
                                          m_rtScene.isDirty() ||
                                          (rtPendingBuildWork && streamingSyncCadenceReached) ||
                                          periodicSyncTick;
        if (shouldSyncChunkCache) {
            size_t maxChunkUploadsPerFrame = 1u;
            const float chunkUploadBudgetMs =
                highPriorityRtWork
                    ? std::max(chunkUploadBudgetMsForTier(m_streamingBudgetTier), 0.85f)
                    : chunkUploadBudgetMsForTier(m_streamingBudgetTier);
            m_sceneUploader.syncChunkCache(
                *scene.chunkWorld.cpuChunkMeshes,
                cullingChunk,
                maxChunkUploadsPerFrame,
                chunkUploadBudgetMs,
                rtActiveRadiusChunks,
                m_frameCounter,
                *m_context,
                m_uploadContext,
                m_rtScene
            );
            m_lastStreamingSyncFrame = m_frameCounter;
        }
        size_t rtBlasUploadsPerFrame = 1u;
        if (m_streamingBudgetTier == kStreamingTierVeryFast) {
            rtBlasUploadsPerFrame = 1u;
        } else if (m_streamingBudgetTier == kStreamingTierFast) {
            rtBlasUploadsPerFrame = 1u;
        } else if (m_streamingBudgetTier == kStreamingTierMedium) {
            rtBlasUploadsPerFrame = 1u;
        }
        if (shouldSyncChunkCache || rtPendingBuildWork) {
            allowOversizedRtBuild =
                m_streamingBudgetTier <= kStreamingTierFast && m_streamingBudgetFrameMsEma < 11.5f;
            rtBlasUploadsSubmitted = m_rtScene.processPendingUploads(
                *m_context,
                m_uploadContext,
                m_frameCounter,
                rtBlasUploadsPerFrame,
                rtBuildPrimitiveBudget,
                rtBuildByteBudget,
                allowOversizedRtBuild,
                *scene.chunkWorld.cpuChunkMeshes
            );
        }
        if (m_rtScene.isDirty()) {
            const bool rtWorkStillPending = m_rtScene.hasPendingBuildWork();
            const bool submittedStreamingBlas = rtBlasUploadsSubmitted > 0u && !highPriorityRtWork;
            const uint64_t minTlasIntervalFrames =
                highPriorityRtWork ? 1u : (rtWorkStillPending ? 8u : 2u);
            const bool canSubmitTlasThisFrame = !submittedStreamingBlas &&
                                                m_frameCounter >=
                                                    (m_lastRtTlasBuildFrame + minTlasIntervalFrames);
            if (canSubmitTlasThisFrame) {
                if (m_rtScene.rebuild(*m_context, m_frameCounter)) {
                    m_lastRtTlasBuildFrame = m_frameCounter;
                    rtTlasDecision = "submitted";
                } else {
                    rtTlasDecision = "rebuild_false";
                }
            } else if (submittedStreamingBlas) {
                rtTlasDecision = "skip_same_frame_blas";
            } else {
                rtTlasDecision = "skip_cadence";
            }
        }
    }
    const auto meshSyncEnd = std::chrono::steady_clock::now();
    m_lastTimingSnapshot.cpuMeshSyncMs = measureMs(meshSyncStart, meshSyncEnd);

    if (m_context) {
        const bool giPendingBuildWork = m_giSceneBuffers.hasPendingBuildWork();
        const uint64_t chunkCacheContentVersion = m_sceneUploader.chunkRenderCache().contentVersion();
        const bool giChunkCacheChanged =
            (chunkCacheContentVersion != m_lastGiChunkCacheContentVersion);
        uint64_t giRebuildIntervalFrames = 6u;
        if (!m_giSceneBuffers.valid()) {
            giRebuildIntervalFrames = 1u;
        } else if (giChunkCacheChanged) {
            // Streaming can touch chunk visibility/upload state every frame while moving.
            // Rebuild GI on cadence instead of every cache tick.
            giRebuildIntervalFrames =
                (m_streamingBudgetTier <= kStreamingTierFast) ? 4u : 8u;
            if (m_recentGiIntegrateMs > 24.0f) {
                giRebuildIntervalFrames = 12u;
            } else if (m_recentGiIntegrateMs > 12.0f) {
                giRebuildIntervalFrames = 8u;
            }
        } else if (giPendingBuildWork) {
            giRebuildIntervalFrames = 2u;
        }
        const bool giRebuildCadenceReached =
            (m_frameCounter >= (m_lastGiRebuildKickFrame + giRebuildIntervalFrames));
        const bool shouldRunGiRebuild =
            !m_giSceneBuffers.valid() ||
            ((giChunkCacheChanged || giPendingBuildWork) && giRebuildCadenceReached);
        if (shouldRunGiRebuild) {
            const bool giWasValid = m_giSceneBuffers.valid();
            const uint64_t framesSinceLastKick = m_frameCounter - m_lastGiRebuildKickFrame;
            const auto giIntegrateStart = std::chrono::steady_clock::now();
            (void)m_giSceneBuffers.rebuild(
                *scene.chunkWorld.chunks,
                m_sceneUploader.chunkRenderCache(),
                *m_context,
                m_frameCounter
            );
            const auto giIntegrateEnd = std::chrono::steady_clock::now();
            m_lastTimingSnapshot.cpuGiIntegrateMs = measureMs(giIntegrateStart, giIntegrateEnd);
            m_recentGiIntegrateMs = m_lastTimingSnapshot.cpuGiIntegrateMs;
            m_lastGiRebuildKickFrame = m_frameCounter;
            m_lastGiChunkCacheContentVersion = chunkCacheContentVersion;
            if (giRebuildLogsEnabled()) {
                std::cout << "[Vulkan][Timing][GiRebuild] frame=" << m_frameCounter
                          << " reason_invalid=" << (!giWasValid ? 1 : 0)
                          << " reason_cpuChunkMeshesChanged=" << (cpuChunkMeshesChanged ? 1 : 0)
                          << " reason_chunkCacheChanged=" << (giChunkCacheChanged ? 1 : 0)
                          << " reason_pendingBuildWork=" << (giPendingBuildWork ? 1 : 0)
                          << " reason_cadence=" << (giRebuildCadenceReached ? 1 : 0)
                          << " intervalFrames=" << giRebuildIntervalFrames
                          << " framesSinceLastKick=" << framesSinceLastKick
                          << " giIntegrateMs=" << m_lastTimingSnapshot.cpuGiIntegrateMs << "\n";
            }
        }
    }

    m_lastCpuChunkMeshesVersion = cpuChunkMeshesVersion;
    m_lastSyncCullingChunk = cullingChunk;
    m_lastSyncCullingChunkValid = true;

    if (timingLogsEnabled()) {
        const uint32_t interval = timingLogInterval();
        if (interval > 0u && (m_frameCounter % interval) == 0u) {
            const VulkanRayTracingScene::StreamingStats rtStats = m_rtScene.streamingStats();
            const auto frameEnd = std::chrono::steady_clock::now();
            const float frameTotalCpuMs = measureMs(frameStart, frameEnd);
            std::cout << "[Vulkan][Timing][Device] frame=" << m_frameCounter
                      << " totalCpuMs=" << frameTotalCpuMs
                      << " frameBuildMs=" << m_lastTimingSnapshot.cpuFrameBuildMs
                      << " rendererMs=" << rendererFrameCpuMs
                      << " cmdRecordMs=" << m_lastTimingSnapshot.cpuCommandRecordMs
                      << " descriptorBinds=" << m_lastTimingSnapshot.descriptorBindCount
                      << " chunkDescriptorBinds="
                      << m_lastTimingSnapshot.chunkDescriptorBindCount
                      << " modelDescriptorBinds="
                      << m_lastTimingSnapshot.modelDescriptorBindCount
                      << " drawIndexedIndirectCalls="
                      << m_lastTimingSnapshot.drawIndexedIndirectCount
                      << " drawIndexedCalls=" << m_lastTimingSnapshot.drawIndexedCount
                      << " chunkPassCpuMs=" << m_lastTimingSnapshot.cpuChunkPassMs
                      << " modelPassCpuMs=" << m_lastTimingSnapshot.cpuModelPassMs
                      << " meshSyncMs=" << m_lastTimingSnapshot.cpuMeshSyncMs
                      << " giIntegrateMs=" << m_lastTimingSnapshot.cpuGiIntegrateMs
                      << " gpuFrameMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuFrameMs : -1.0f)
                      << " gpuChunkMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuChunkPassMs : -1.0f)
                      << " gpuMainSetupMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuMainSetupMs : -1.0f)
                      << " gpuChunkDrawMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuChunkDrawMs : -1.0f)
                      << " gpuModelMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuModelPassMs : -1.0f)
                      << " gpuMainTailMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuMainTailMs : -1.0f)
                      << " gpuMainMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuMainPassMs : -1.0f)
                      << " gpuNrdDispatchMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuNrdDispatchMs
                                                        : -1.0f)
                      << " gpuCompositeMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuCompositePassMs
                                                        : -1.0f)
                      << " gpuUiMs="
                      << (m_lastTimingSnapshot.gpuValid ? m_lastTimingSnapshot.gpuUiPassMs : -1.0f)
                      << " chunkBatches=" << m_lastTimingSnapshot.chunkBatchCount
                      << " chunkCommands=" << m_lastTimingSnapshot.chunkCommandCount
                      << " chunkInstances=" << m_lastTimingSnapshot.chunkInstanceCount
                      << " chunkIndices=" << m_lastTimingSnapshot.chunkIndexCountTotal
                      << " chunkIndexInstances="
                      << m_lastTimingSnapshot.chunkIndexInstanceCountTotal
                      << " chunkTriangles=" << m_lastTimingSnapshot.chunkTriangleCountTotal
                      << " chunkSuperbatch=" << (m_frameData.chunkSuperbatchEnabled ? 1 : 0)
                      << " sceneObjects=" << m_lastTimingSnapshot.objectCount
                      << " modelMatrices=" << m_lastTimingSnapshot.modelMatrixCount
                      << " giBackend="
                      << ((m_lastTimingSnapshot.giTracingBackend == GiTracingBackend::HardwareRt)
                              ? "HardwareRt"
                              : "SoftwareDda")
                      << " giRays=" << m_lastTimingSnapshot.giRaysCast
                      << " rtSceneReady=" << (m_lastTimingSnapshot.giRtSceneReady ? "1" : "0")
                      << " streamingTier=" << static_cast<uint32_t>(m_streamingBudgetTier)
                      << " streamingEmaMs=" << m_streamingBudgetFrameMsEma
                      << " rtRadius=" << rtActiveRadiusChunks
                      << " rtPrimBudget=" << rtBuildPrimitiveBudget
                      << " rtByteBudget=" << rtBuildByteBudget
                      << " rtAllowOversized=" << (allowOversizedRtBuild ? 1 : 0)
                      << " rtPendingNormal=" << rtStats.pendingNormalUploads
                      << " rtPendingHigh=" << rtStats.pendingHighPriorityUploads
                      << " rtPendingTracked=" << rtStats.pendingTrackedUploads
                      << " rtPendingGpuBatches=" << rtStats.pendingGpuBuildBatches
                      << " rtPendingTlas=" << (rtStats.pendingTlasBuild ? 1 : 0)
                      << " rtCachedBlas=" << rtStats.cachedBlasCount
                      << " rtActiveBlas=" << rtStats.activeBlasCount
                      << " rtBlasSubmittedThisFrame=" << rtBlasUploadsSubmitted
                      << " rtLastBuildFrame=" << rtStats.lastBuildFrame
                      << " rtLastBuildSubmitted=" << rtStats.lastBuildSubmitted
                      << " rtSelectedPrims=" << rtStats.lastBuildSelectedPrimitives
                      << " rtSelectedBytes=" << rtStats.lastBuildSelectedBytes
                      << " rtDeferredBudget=" << (rtStats.lastBuildDeferredBudget ? 1 : 0)
                      << " rtDeferredOversized=" << (rtStats.lastBuildDeferredOversized ? 1 : 0)
                      << " rtDeferredPrims=" << rtStats.lastBuildDeferredPrimitives
                      << " rtDeferredBytes=" << rtStats.lastBuildDeferredBytes
                      << " rtTlasDecision=" << rtTlasDecision
                      << " rtLastTlasFrame=" << rtStats.lastTlasFrame
                      << " rtLastTlasSubmitted=" << (rtStats.lastTlasSubmitted ? 1 : 0)
                      << " rtLastTlasInstances=" << rtStats.lastTlasInstanceCount
                      << " rtTlasSkippedPending=" << (rtStats.lastTlasSkippedPendingBuild ? 1 : 0)
                      << " rtTlasSkippedEmpty=" << (rtStats.lastTlasSkippedEmpty ? 1 : 0)
                      << "\n";
        }
    }
}

void VulkanRenderDevice::shutdown() {
    if (!m_initialized && !m_context && !m_renderer) {
        return;
    }

    if (m_renderer) {
        m_renderer->cleanup();
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
    m_giSceneBuffers.cleanup();
    m_uploadContext.cleanup();

    if (m_renderer) {
        m_renderer.reset();
    }
    if (m_context) {
        m_context->cleanup();
        m_context.reset();
    }

    m_window = nullptr;
    m_initialized = false;
    m_lastRtTlasBuildFrame = 0;
    m_lastStreamingSyncFrame = 0;
    m_lastGiRebuildKickFrame = 0;
    m_lastGiChunkCacheContentVersion = 0;
    m_recentGiIntegrateMs = 0.0f;
    m_lastCpuChunkMeshesVersion = 0;
    m_lastSyncCullingChunk = glm::ivec3(0);
    m_lastSyncCullingChunkValid = false;
    m_streamingBudgetFrameMsEma = 0.0f;
    m_streamingBudgetFrameMsEmaValid = false;
    m_streamingBudgetTier = kStreamingTierMedium;
    m_streamingBudgetTierHoldFrames = 0;
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
