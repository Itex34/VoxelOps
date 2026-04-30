#pragma once

#include <SDL3/SDL.h>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

struct ImDrawData;

struct UiFrameData {
    float fps = 0.0f;
    float frameMs = 0.0f;
    glm::vec3 playerPosition{0.0f};
    glm::vec3 playerVelocity{0.0f};
    bool flyMode = false;
    bool onGround = false;
    uint16_t renderDistance = 0;
    size_t remotePlayerCount = 0;
    bool netConnected = false;
    std::string_view netStatus{};
    uint32_t serverTick = 0;
    uint32_t ackedInputTick = 0;
    size_t pendingInputCount = 0;
    size_t chunkDataQueueDepth = 0;
    size_t chunkDeltaQueueDepth = 0;
    size_t chunkUnloadQueueDepth = 0;
    std::string_view backendName{};
    bool mdiUsable = false;
    float perfFrameCpuMs = 0.0f;
    float perfInputMs = 0.0f;
    float perfNetworkMs = 0.0f;
    float perfPredictionMs = 0.0f;
    float perfGameplayMs = 0.0f;
    float perfRenderCpuMs = 0.0f;
    float perfPresentMs = 0.0f;
    float perfChunkStreamingMs = 0.0f;
    bool vulkanTimingValid = false;
    float vkCpuCommandRecordMs = 0.0f;
    float vkCpuChunkPassMs = 0.0f;
    float vkCpuModelPassMs = 0.0f;
    float vkCpuUiPassMs = 0.0f;
    float vkCpuMeshSyncMs = 0.0f;
    float vkCpuFrameBuildMs = 0.0f;
    float vkCpuGiIntegrateMs = 0.0f;
    uint32_t vkGiTraceGridsUpdated = 0;
    uint64_t vkGiRaysCast = 0;
    float vkGiAverageIrradianceLuma = 0.0f;
    bool vkGpuTimingValid = false;
    float vkGpuFrameMs = 0.0f;
    float vkGpuChunkPassMs = 0.0f;
    float vkGpuModelPassMs = 0.0f;
    float vkGpuUiPassMs = 0.0f;
    float vkGpuGiIntegrateMs = 0.0f;
    bool vkGiHardwareRtSupported = false;
    bool vkGiRtSceneReady = false;
    int vkGiTracingBackend = 0; // 0=SoftwareDda, 1=HardwareRt
    bool vkNrdBootstrapActive = false;
    uint32_t vkNrdBootstrapDispatchCount = 0;
};

struct UiMutableState {
    bool *useDebugCamera = nullptr;
    bool *toggleWireframe = nullptr;
    bool *toggleChunkBorders = nullptr;
    bool *toggleDebugFrustum = nullptr;
    uint16_t *renderDistance = nullptr;
    bool *cursorEnabled = nullptr;
    bool *rawMouseInputEnabled = nullptr;
    bool rawMouseInputSupported = true;
    glm::vec3 *gunViewOffset = nullptr;
    glm::vec3 *gunViewScale = nullptr;
    glm::vec3 *gunViewEulerDeg = nullptr;
    glm::vec3 *sunDirection = nullptr;
    glm::vec3 *sunShadowDirectionalBias = nullptr; // x=+Y, y=side, z=-Y
    float *sunShadowLowSunBiasBoost = nullptr;
    bool *sunShadowFrontFaceCullAtLowSun = nullptr;
    float *sunShadowFrontFaceCullGrazingThreshold = nullptr;
    float *skyExposure = nullptr;
    // 0 = Auto, 1 = Software DDA, 2 = Hardware RT
    int *giTracingBackendPreference = nullptr;
    // 0 = Off, 1 = Diff Radiance, 2 = Hit Distance, 3 = Normal, 4 = Motion, 5 = ViewZ
    int *giNrdDebugView = nullptr;
};

struct UiVulkanInitInfo {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t minImageCount = 2;
    uint32_t imageCount = 2;
    uint32_t apiVersion = VK_API_VERSION_1_3;
};

class DebugUi {
  public:
    bool initialize(SDL_Window *window, SDL_GLContext glContext, const char *glslVersion);
    bool initializeForVulkan(SDL_Window *window, const UiVulkanInitInfo &initInfo);
    void processEvent(const SDL_Event &event);
    void shutdown();

    void beginFrame();
    void drawCrosshair(bool enabled);
    void drawMainWindow(const UiFrameData &data, UiMutableState &state);
    ImDrawData *endFrame();
    void renderDrawData(ImDrawData *drawData);

    void setVisible(bool visible) noexcept;
    void toggleVisible() noexcept;
    [[nodiscard]] bool isVisible() const noexcept;

  private:
    enum class BackendType : uint8_t { None = 0, OpenGL = 1, Vulkan = 2 };

    bool m_initialized = false;
    bool m_visible = true;
    bool m_showDemoWindow = false;
    bool m_crosshairEnabled = true;
    BackendType m_backendType = BackendType::None;
};
