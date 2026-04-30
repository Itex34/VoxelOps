#pragma once

#include "IRenderDevice.hpp"

#include "../world/ChunkManager.hpp"
#include "Vulkan/VulkanGiSettings.hpp"
#include "Vulkan/VulkanGiSceneBuffers.hpp"
#include "Vulkan/VulkanSceneUploader.hpp"
#include "Vulkan/VulkanRayTracingScene.hpp"
#include "Vulkan/renderer/RenderFrameData.hpp"
#include "Vulkan/vulkan/UploadContext.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <memory>

class VulkanContext;
class VulkanRenderer;
class Camera;
class Player;
struct ImDrawData;

class VulkanRenderDevice final : public IRenderDevice {
  public:
    struct TimingSnapshot {
        bool gpuValid = false;
        float gpuFrameMs = 0.0f;
        float gpuChunkPassMs = 0.0f;
        float gpuModelPassMs = 0.0f;
        float gpuUiPassMs = 0.0f;

        float cpuCommandRecordMs = 0.0f;
        float cpuChunkPassMs = 0.0f;
        float cpuModelPassMs = 0.0f;
        float cpuUiPassMs = 0.0f;

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

    void renderFrame(RenderFrameParams &params) override;
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

    VulkanSceneUploader m_sceneUploader;
    FrameRenderData m_frameData;
    VulkanRayTracingScene m_rtScene;
    VulkanGiSettings m_giSettings;
    VulkanGiSceneBuffers m_giSceneBuffers;
    TimingSnapshot m_lastTimingSnapshot{};
};
