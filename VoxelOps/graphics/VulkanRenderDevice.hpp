#pragma once

#include "IRenderDevice.hpp"

#include "Vulkan/VulkanGiSettings.hpp"
#include "Vulkan/VulkanGiSceneBuffers.hpp"
#include "Vulkan/VulkanSceneUploader.hpp"
#include "Vulkan/VulkanRayTracingScene.hpp"
#include "Vulkan/renderer/RenderFrameData.hpp"
#include "Vulkan/vulkan/UploadContext.hpp"

#include <SDL3/SDL.h>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>

class VulkanContext;
class VulkanRenderer;
class Camera;
struct ImDrawData;

class VulkanRenderDevice final : public IRenderDevice {
public:
    struct TimingSnapshot {
        bool gpuValid = false;
        float gpuFrameMs = 0.0f;
        float gpuChunkPassMs = 0.0f;
        float gpuMainSetupMs = 0.0f;
        float gpuChunkDrawMs = 0.0f;
        float gpuModelPassMs = 0.0f;
        float gpuUiPassMs = 0.0f;
        float gpuMainTailMs = 0.0f;
        float gpuMainPassMs = 0.0f;
        float gpuNrdDispatchMs = 0.0f;
        float gpuCompositePassMs = 0.0f;

        float cpuCommandRecordMs = 0.0f;
        float cpuChunkPassMs = 0.0f;
        float cpuModelPassMs = 0.0f;
        float cpuUiPassMs = 0.0f;
        uint32_t descriptorBindCount = 0;
        uint32_t chunkDescriptorBindCount = 0;
        uint32_t modelDescriptorBindCount = 0;
        uint32_t drawIndexedIndirectCount = 0;
        uint32_t drawIndexedCount = 0;

        float cpuMeshSyncMs = 0.0f;
        float cpuFrameBuildMs = 0.0f;
        float cpuGiIntegrateMs = 0.0f;
        float gpuGiIntegrateMs = 0.0f;
        uint32_t giTraceGridsUpdated = 0;
        uint64_t giRaysCast = 0;
        float giAverageIrradianceLuma = 0.0f;
        bool giHardwareRtSupported = false;
        bool giRtSceneReady = false;
        GiTracingBackend giTracingBackend = GiTracingBackend::SoftwareDda;
        bool nrdBootstrapActive = false;
        uint32_t nrdBootstrapDispatchCount = 0;
        uint32_t chunkBatchCount = 0;
        uint32_t chunkCommandCount = 0;
        uint32_t chunkInstanceCount = 0;
        uint64_t chunkIndexCountTotal = 0;
        uint64_t chunkIndexInstanceCountTotal = 0;
        uint64_t chunkTriangleCountTotal = 0;
        uint32_t objectCount = 0;
        uint32_t modelMatrixCount = 0;
    };

    VulkanRenderDevice();
    ~VulkanRenderDevice() override;

    bool initialize(SDL_Window *window) override;

    VkInstance getVkInstanceHandle() const noexcept;
    VkPhysicalDevice getVkPhysicalDeviceHandle() const noexcept;
    VkDevice getVkDeviceHandle() const noexcept;
    uint32_t getVkGraphicsQueueFamily() const noexcept;
    VkQueue getVkGraphicsQueueHandle() const noexcept;
    VkRenderPass getVkRenderPassHandle() const noexcept;
    uint32_t getVkSwapchainImageCount() const noexcept;
    const TimingSnapshot &getLastTimingSnapshot() const noexcept {
        return m_lastTimingSnapshot;
    }

    RenderDeviceCapabilities getCapabilities() const noexcept override;

    void renderFrame(RenderScene &scene) override;
    void onWindowResized(int width, int height) override;
    bool initializeDebugUi(DebugUi &debugUi, SDL_Window *window, void *nativeContext) override;
    void appendBackendDebugUiFrameData(UiFrameData &frameData) const override;
    void shutdown() override;

private:
    bool ensureInitialized();

    SDL_Window *m_window = nullptr;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<VulkanRenderer> m_renderer;
    UploadContext m_uploadContext;

    bool m_initialized = false;
    bool m_warnedUninitializedRender = false;
    bool m_warnedNoCpuChunkMeshes = false;
    uint64_t m_frameCounter = 0;
    uint64_t m_lastRtTlasBuildFrame = 0;
    uint64_t m_lastStreamingSyncFrame = 0;
    uint64_t m_lastGiRebuildKickFrame = 0;
    uint64_t m_lastGiChunkCacheContentVersion = 0;
    float m_recentGiIntegrateMs = 0.0f;
    uint64_t m_lastCpuChunkMeshesVersion = 0;
    glm::ivec3 m_lastSyncCullingChunk{0};
    bool m_lastSyncCullingChunkValid = false;
    float m_streamingBudgetFrameMsEma = 0.0f;
    bool m_streamingBudgetFrameMsEmaValid = false;
    uint8_t m_streamingBudgetTier = 2u;
    uint32_t m_streamingBudgetTierHoldFrames = 0;

    VulkanSceneUploader m_sceneUploader;
    FrameRenderData m_frameData;
    VulkanRayTracingScene m_rtScene;
    VulkanGiSettings m_giSettings;
    VulkanGiSceneBuffers m_giSceneBuffers;
    TimingSnapshot m_lastTimingSnapshot{};
};
