#pragma once

#include "IRenderDevice.hpp"

#include "ChunkManager.hpp"
#include "Vulkan/graphics/Mesh.hpp"
#include "Vulkan/graphics/Model.hpp"
#include "Vulkan/graphics/Texture.hpp"
#include "Vulkan/renderer/GiClipmapPlan.hpp"
#include "Vulkan/renderer/RenderFrameData.hpp"
#include "Vulkan/vulkan/UploadContext.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <unordered_map>
#include <vector>

class VulkanContext;
class VulkanRenderer;
class Camera;
class Player;

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
        uint32_t giProbesUpdated = 0;
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

    bool initialize(SDL_Window* window);
    void onWindowResized(int width, int height);
    void renderFrameVulkan(
        ChunkManager& chunkManager,
        const Camera& activeCamera,
        const Camera& cullingCamera,
        const Player& player,
        const glm::vec3& sunDirection);
    VkInstance getVkInstanceHandle() const noexcept;
    VkPhysicalDevice getVkPhysicalDeviceHandle() const noexcept;
    VkDevice getVkDeviceHandle() const noexcept;
    uint32_t getVkGraphicsQueueFamily() const noexcept;
    VkQueue getVkGraphicsQueueHandle() const noexcept;
    VkRenderPass getVkRenderPassHandle() const noexcept;
    uint32_t getVkSwapchainImageCount() const noexcept;
    const TimingSnapshot& getLastTimingSnapshot() const noexcept { return m_lastTimingSnapshot; }

    int getOpenGLVersionMajor() const noexcept override;
    int getOpenGLVersionMinor() const noexcept override;
    GraphicsBackend getActiveBackend() const noexcept override;
    std::string_view getActiveBackendName() const noexcept override;
    bool isMDIUsable() const noexcept override;

    void renderFrame(RenderFrameParams& params) override;
    void shutdown() override;

    std::string_view getApiName() const noexcept override;

private:
    struct VulkanChunkMesh {
        VkMesh mesh;
        uint64_t revision = 0;
    };
    struct RetiredChunkMesh {
        VkMesh mesh;
        uint64_t retireFrame = 0;
    };
    struct GiRuntimeStats {
        uint32_t probesUpdated = 0;
        uint64_t raysCast = 0;
        float cpuIntegrateMs = 0.0f;
        float gpuIntegrateMs = 0.0f;
        float averageIrradianceLuma = 0.0f;
    };
    struct GiComputeState;
    struct RtSceneState;

    bool ensureInitialized();
    bool ensureAtlasTextureLoaded();
    bool ensureRemotePlayerAssetsLoaded();
    void initGiClipmaps();
    void resetGiClipmaps();
    bool initGiComputeResources();
    void resetGiComputeResources();
    bool dispatchGiProbeCompute(
        ChunkManager& chunkManager,
        const GiClipmapPlan::GiFrameBudget& frameBudget,
        uint32_t cascadeCount
    );
    void updateGiProbes(ChunkManager& chunkManager, const glm::vec3& probeAnchorPosition);
    void cleanupRemotePlayerAssets();
    void syncChunkMeshes(ChunkManager& chunkManager, const glm::ivec3& cullingChunk);
    void retireChunkMesh(VkMesh&& mesh);
    void collectRetiredChunkMeshes();
    void initRayTracingScene();
    void collectRetiredRayTracingResources();
    void resetRayTracingScene();
    bool uploadChunkRayTracingGeometry(const glm::ivec3& chunkPos, const CpuChunkMesh& cpuMesh);
    void removeChunkRayTracingGeometry(const glm::ivec3& chunkPos);
    bool rebuildRayTracingScene();
    void cleanupChunkMeshes();

    SDL_Window* m_window = nullptr;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<VulkanRenderer> m_renderer;
    UploadContext m_uploadContext;
    VkTexture m_atlasTexture;
    bool m_atlasTextureLoaded = false;
    std::unique_ptr<VkModel> m_remotePlayerModel;
    std::vector<VkTexture> m_remotePlayerTextures;
    std::vector<const VkTexture*> m_remotePlayerTextureViews;
    bool m_remotePlayerAssetsLoaded = false;
    bool m_warnedRemotePlayerAssets = false;

    bool m_initialized = false;
    bool m_warnedUninitializedRender = false;
    bool m_warnedNoCpuChunkMeshes = false;
    uint64_t m_frameCounter = 0;

    std::unordered_map<glm::ivec3, VulkanChunkMesh, IVec3Hash> m_chunkMeshes;
    std::vector<RetiredChunkMesh> m_retiredChunkMeshes;
    FrameRenderData m_frameData;
    GiClipmapPlan::GiClipmapConfig m_giConfig = GiClipmapPlan::makeDefaultConfig();
    std::array<GiClipmapPlan::GiCascadeRuntimeState, GiClipmapPlan::MAX_CASCADES> m_giCascadeRuntime{};
    std::unique_ptr<GiComputeState> m_giComputeState;
    std::unique_ptr<RtSceneState> m_rtSceneState;
    bool m_rtSceneDirty = false;
    VkAccelerationStructureKHR m_activeGiSceneTlas = VK_NULL_HANDLE;
    glm::vec3 m_giSunDirection = glm::vec3(0.25f, 0.85f, 0.42f);
    float m_giSunIntensity = 1.10f;
    bool m_giHistoryAnchorValid = false;
    glm::vec3 m_giHistoryAnchor = glm::vec3(0.0f);
    glm::vec3 m_prevGiHistorySunDir = glm::vec3(0.25f, 0.85f, 0.42f);
    bool m_lastGiLightingValid = false;
    GiLightingData m_lastGiLighting{};
    bool m_lastTraceSceneValid = false;
    VkBuffer m_lastTraceOccupancyBuffer = VK_NULL_HANDLE;
    VkBuffer m_lastTraceMaterialBuffer = VK_NULL_HANDLE;
    glm::ivec3 m_lastTraceOccupancyMinBlocks{ 0 };
    glm::uvec3 m_lastTraceOccupancyDims{ 0u };
    uint32_t m_lastTraceOccupancyWordCount = 0;
    float m_lastTraceSunShadowMaxDistance = 64.0f;
    GiRuntimeStats m_lastGiStats{};
    TimingSnapshot m_lastTimingSnapshot{};
    bool m_warnedHardwareRtUnavailable = false;
    bool m_loggedGiTracingBackend = false;
    GiTracingBackend m_lastGiTracingBackend = GiTracingBackend::SoftwareDda;
};


