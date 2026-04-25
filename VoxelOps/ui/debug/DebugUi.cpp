#include "DebugUi.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <imgui.h>
#if __has_include(<imgui_impl_sdl3.h>)
#define VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE 1
#include <imgui_impl_sdl3.h>
#else
#define VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE 0
#endif

#if __has_include(<imgui_impl_opengl3.h>)
#define VOXELOPS_IMGUI_OPENGL_BACKEND_AVAILABLE 1
#include <imgui_impl_opengl3.h>
#else
#define VOXELOPS_IMGUI_OPENGL_BACKEND_AVAILABLE 0
#endif

#if __has_include(<imgui_impl_vulkan.h>)
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 1
#include <imgui_impl_vulkan.h>
#else
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 0
#endif

bool DebugUi::initialize(SDL_Window *window, SDL_GLContext glContext, const char *glslVersion) {
    if (m_initialized) {
        return true;
    }

#if !VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE || !VOXELOPS_IMGUI_OPENGL_BACKEND_AVAILABLE
    (void)window;
    (void)glContext;
    (void)glslVersion;
    return false;
#else

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_initialized = true;
    m_backendType = BackendType::OpenGL;
    return true;
#endif
}

bool DebugUi::initializeForVulkan(SDL_Window *window, const UiVulkanInitInfo &initInfo) {
    if (m_initialized) {
        return true;
    }

#if !VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE || !VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    (void)window;
    (void)initInfo;
    return false;
#else
    if (window == nullptr || initInfo.instance == VK_NULL_HANDLE ||
        initInfo.physicalDevice == VK_NULL_HANDLE || initInfo.device == VK_NULL_HANDLE ||
        initInfo.queue == VK_NULL_HANDLE || initInfo.renderPass == VK_NULL_HANDLE ||
        initInfo.imageCount < 2) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplVulkan_InitInfo backendInfo{};
    backendInfo.ApiVersion = initInfo.apiVersion;
    backendInfo.Instance = initInfo.instance;
    backendInfo.PhysicalDevice = initInfo.physicalDevice;
    backendInfo.Device = initInfo.device;
    backendInfo.QueueFamily = initInfo.queueFamily;
    backendInfo.Queue = initInfo.queue;
    backendInfo.DescriptorPool = VK_NULL_HANDLE;
    backendInfo.DescriptorPoolSize = 256;
    backendInfo.MinImageCount = std::max(2u, initInfo.minImageCount);
    backendInfo.ImageCount = std::max(backendInfo.MinImageCount, initInfo.imageCount);
    backendInfo.PipelineCache = VK_NULL_HANDLE;
    backendInfo.PipelineInfoMain.RenderPass = initInfo.renderPass;
    backendInfo.PipelineInfoMain.Subpass = 0;
    backendInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    backendInfo.UseDynamicRendering = false;
    backendInfo.Allocator = nullptr;
    backendInfo.CheckVkResultFn = nullptr;
    backendInfo.MinAllocationSize = 1024 * 1024;

    if (!ImGui_ImplVulkan_Init(&backendInfo)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_initialized = true;
    m_backendType = BackendType::Vulkan;
    return true;
#endif
}

void DebugUi::shutdown() {
    if (!m_initialized) {
        return;
    }
#if VOXELOPS_IMGUI_OPENGL_BACKEND_AVAILABLE
    if (m_backendType == BackendType::OpenGL) {
        ImGui_ImplOpenGL3_Shutdown();
    }
#endif
#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    if (m_backendType == BackendType::Vulkan) {
        ImGui_ImplVulkan_Shutdown();
    }
#endif
#if VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE
    ImGui_ImplSDL3_Shutdown();
#endif
    ImGui::DestroyContext();
    m_initialized = false;
    m_backendType = BackendType::None;
}

void DebugUi::processEvent(const SDL_Event &event) {
    if (!m_initialized) {
        return;
    }
#if VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE
    ImGui_ImplSDL3_ProcessEvent(&event);
#else
    (void)event;
#endif
}

void DebugUi::beginFrame() {
    if (!m_initialized) {
        return;
    }
#if VOXELOPS_IMGUI_OPENGL_BACKEND_AVAILABLE
    if (m_backendType == BackendType::OpenGL) {
        ImGui_ImplOpenGL3_NewFrame();
    }
#endif
#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    if (m_backendType == BackendType::Vulkan) {
        ImGui_ImplVulkan_NewFrame();
    }
#endif
#if VOXELOPS_IMGUI_SDL_BACKEND_AVAILABLE
    ImGui_ImplSDL3_NewFrame();
#endif
    ImGui::NewFrame();
}

void DebugUi::drawCrosshair(bool enabled) {
    if (!m_initialized || !enabled || !m_crosshairEnabled) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImDrawList *drawList = ImGui::GetForegroundDrawList();

    constexpr float kGap = 5.0f;
    constexpr float kArm = 8.0f;
    constexpr float kThickness = 2.0f;
    const ImU32 color = IM_COL32(245, 245, 245, 230);

    drawList->AddLine(ImVec2(center.x - (kGap + kArm), center.y), ImVec2(center.x - kGap, center.y),
                      color, kThickness);
    drawList->AddLine(ImVec2(center.x + kGap, center.y), ImVec2(center.x + (kGap + kArm), center.y),
                      color, kThickness);
    drawList->AddLine(ImVec2(center.x, center.y - (kGap + kArm)), ImVec2(center.x, center.y - kGap),
                      color, kThickness);
    drawList->AddLine(ImVec2(center.x, center.y + kGap), ImVec2(center.x, center.y + (kGap + kArm)),
                      color, kThickness);
}

void DebugUi::drawMainWindow(const UiFrameData &data, UiMutableState &state) {
    if (!m_initialized || !m_visible) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VoxelOps Debug", &m_visible)) {
        ImGui::End();
        return;
    }

    const std::string_view backendName =
        data.backendName.empty() ? std::string_view("unknown") : data.backendName;

    ImGui::Text("FPS: %.1f (%.2f ms)", data.fps, data.frameMs);
    ImGui::Text("Backend: %.*s | MDI: %s", static_cast<int>(backendName.size()), backendName.data(),
                data.mdiUsable ? "yes" : "no");

    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr size_t kHistory = 240;
        static std::array<float, kHistory> s_histFrame{};
        static std::array<float, kHistory> s_histInput{};
        static std::array<float, kHistory> s_histNetwork{};
        static std::array<float, kHistory> s_histPrediction{};
        static std::array<float, kHistory> s_histGameplay{};
        static std::array<float, kHistory> s_histRender{};
        static std::array<float, kHistory> s_histPresent{};
        static std::array<float, kHistory> s_histChunk{};
        static size_t s_histWriteIndex = 0;

        s_histFrame[s_histWriteIndex] = data.perfFrameCpuMs;
        s_histInput[s_histWriteIndex] = data.perfInputMs;
        s_histNetwork[s_histWriteIndex] = data.perfNetworkMs;
        s_histPrediction[s_histWriteIndex] = data.perfPredictionMs;
        s_histGameplay[s_histWriteIndex] = data.perfGameplayMs;
        s_histRender[s_histWriteIndex] = data.perfRenderCpuMs;
        s_histPresent[s_histWriteIndex] = data.perfPresentMs;
        s_histChunk[s_histWriteIndex] = data.perfChunkStreamingMs;
        s_histWriteIndex = (s_histWriteIndex + 1) % kHistory;

        const float accountedMs = data.perfInputMs + data.perfNetworkMs + data.perfPredictionMs +
                                  data.perfGameplayMs + data.perfRenderCpuMs + data.perfPresentMs +
                                  data.perfChunkStreamingMs;
        const float denomMs = std::max(data.perfFrameCpuMs, 0.001f);
        const auto pct = [denomMs](float ms) { return (ms / denomMs) * 100.0f; };
        const float graphMaxMs = std::max(8.0f, std::max(data.perfFrameCpuMs * 1.5f, 16.0f));

        ImGui::Text("CPU frame: %.3f ms | Accounted: %.3f ms (%.1f%%)", data.perfFrameCpuMs,
                    accountedMs, pct(accountedMs));
        ImGui::PlotLines("CPU frame ms", s_histFrame.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 70.0f));
        ImGui::PlotLines("Input/UI", s_histInput.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));
        ImGui::PlotLines("Network/Reconcile", s_histNetwork.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));
        ImGui::PlotLines("Prediction", s_histPrediction.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));
        ImGui::PlotLines("Gameplay", s_histGameplay.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));
        ImGui::PlotLines("Render CPU", s_histRender.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));
        ImGui::PlotLines("Present (swap+poll)", s_histPresent.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));
        ImGui::PlotLines("Chunk streaming", s_histChunk.data(), static_cast<int>(kHistory),
                         static_cast<int>(s_histWriteIndex), nullptr, 0.0f, graphMaxMs,
                         ImVec2(0.0f, 46.0f));

        ImGui::Text("Breakdown ms: in %.3f | net %.3f | pred %.3f | game %.3f | ren %.3f | present "
                    "%.3f | chunk %.3f",
                    data.perfInputMs, data.perfNetworkMs, data.perfPredictionMs,
                    data.perfGameplayMs, data.perfRenderCpuMs, data.perfPresentMs,
                    data.perfChunkStreamingMs);

        if (data.vulkanTimingValid) {
            ImGui::Separator();
            ImGui::TextUnformatted("Vulkan CPU");
            ImGui::Text("record %.3f | chunk-pass %.3f | model-pass %.3f | ui-pass %.3f",
                        data.vkCpuCommandRecordMs, data.vkCpuChunkPassMs, data.vkCpuModelPassMs,
                        data.vkCpuUiPassMs);
            ImGui::Text("mesh-sync %.3f | frame-build %.3f", data.vkCpuMeshSyncMs,
                        data.vkCpuFrameBuildMs);
            ImGui::Text("gi integrate %.3f | probes %u | rays %llu | gi luma %.3f",
                        data.vkCpuGiIntegrateMs, data.vkGiProbesUpdated,
                        static_cast<unsigned long long>(data.vkGiRaysCast),
                        data.vkGiAverageIrradianceLuma);
        }
        if (data.vkGpuTimingValid) {
            ImGui::Separator();
            ImGui::TextUnformatted("Vulkan GPU");
            ImGui::Text("frame %.3f | chunk-pass %.3f | model-pass %.3f | ui-pass %.3f | gi %.3f",
                        data.vkGpuFrameMs, data.vkGpuChunkPassMs, data.vkGpuModelPassMs,
                        data.vkGpuUiPassMs, data.vkGpuGiIntegrateMs);
        }
    }

    ImGui::Separator();
    ImGui::Text("Player");
    ImGui::Text("Pos: (%.2f, %.2f, %.2f)", data.playerPosition.x, data.playerPosition.y,
                data.playerPosition.z);
    ImGui::Text("Vel: (%.2f, %.2f, %.2f)", data.playerVelocity.x, data.playerVelocity.y,
                data.playerVelocity.z);
    ImGui::Text("Fly mode: %s | Grounded: %s", data.flyMode ? "on" : "off",
                data.onGround ? "yes" : "no");
    ImGui::Text("Remote players: %zu", data.remotePlayerCount);

    if (state.renderDistance != nullptr) {
        int renderDistanceInt = static_cast<int>(*state.renderDistance);
        if (ImGui::SliderInt("Render Distance", &renderDistanceInt, 2, 24)) {
            renderDistanceInt = std::clamp(renderDistanceInt, 2, 24);
            *state.renderDistance = static_cast<uint16_t>(renderDistanceInt);
        }
    }

    ImGui::Separator();
    ImGui::Text("Render Toggles");
    if (state.useDebugCamera != nullptr) {
        ImGui::Checkbox("Debug Camera (F1)", state.useDebugCamera);
    }
    if (state.toggleWireframe != nullptr) {
        ImGui::Checkbox("Wireframe (T)", state.toggleWireframe);
    }
    if (state.toggleChunkBorders != nullptr) {
        ImGui::Checkbox("Chunk Borders (F2)", state.toggleChunkBorders);
    }
    if (state.toggleDebugFrustum != nullptr) {
        ImGui::Checkbox("Debug Frustum (F3)", state.toggleDebugFrustum);
    }
    if (state.skyExposure != nullptr) {
        ImGui::SliderFloat("Sky Exposure", state.skyExposure, 0.05f, 8.0f, "%.2f");
    }
    if (state.sunDirection != nullptr) {
        ImGui::SliderFloat("Sun Dir X", &state.sunDirection->x, -1.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Sun Dir Y", &state.sunDirection->y, -1.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Sun Dir Z", &state.sunDirection->z, -1.0f, 1.0f, "%.3f");
    }
    if (state.giTracingBackendPreference != nullptr) {
        int mode = std::clamp(*state.giTracingBackendPreference, 0, 2);
        const char *labels[] = {"Auto", "Software DDA", "Hardware RT"};
        if (ImGui::Combo("GI Tracing Path", &mode, labels, IM_ARRAYSIZE(labels))) {
            *state.giTracingBackendPreference = mode;
        }
        if (data.vulkanTimingValid) {
            ImGui::Text("Active GI backend: %s | RT support: %s | RT scene: %s",
                        (data.vkGiTracingBackend == 1) ? "Hardware RT" : "Software DDA",
                        data.vkGiHardwareRtSupported ? "yes" : "no",
                        data.vkGiRtSceneReady ? "ready" : "not ready");
            ImGui::Text("NRD bootstrap: %s | Dispatches: %u",
                        data.vkNrdBootstrapActive ? "active" : "inactive",
                        data.vkNrdBootstrapDispatchCount);
            if (mode == 2 && (!data.vkGiHardwareRtSupported || !data.vkGiRtSceneReady)) {
                ImGui::TextUnformatted(
                    "Requested Hardware RT is unavailable; using Software DDA fallback.");
            }
        }
    }
    if (state.giNrdDebugView != nullptr) {
        int viewMode = std::clamp(*state.giNrdDebugView, 0, 5);
        const char *labels[] = {"Off",    "Diff Radiance", "Hit Distance",
                                "Normal", "Motion",        "ViewZ"};
        if (ImGui::Combo("NRD Input Debug", &viewMode, labels, IM_ARRAYSIZE(labels))) {
            *state.giNrdDebugView = viewMode;
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Sun Shadow Bias")) {
        ImGui::TextUnformatted("Directional receiver bias (depth compare).");
        ImGui::TextUnformatted(
            "Axes: +Y = upward faces, Side = vertical faces, -Y = downward faces.");

        if (state.sunShadowDirectionalBias != nullptr) {
            ImGui::SliderFloat("Bias +Y", &state.sunShadowDirectionalBias->x, 0.0f, 0.00080f,
                               "%.6f");
            ImGui::SliderFloat("Bias Side", &state.sunShadowDirectionalBias->y, 0.0f, 0.00030f,
                               "%.6f");
            ImGui::SliderFloat("Bias -Y", &state.sunShadowDirectionalBias->z, 0.0f, 0.00030f,
                               "%.6f");
            if (state.sunShadowLowSunBiasBoost != nullptr) {
                ImGui::SliderFloat("Low Sun Bias Boost", state.sunShadowLowSunBiasBoost, 0.0f, 8.0f,
                                   "%.2f");
            }
            if (state.sunShadowFrontFaceCullAtLowSun != nullptr) {
                ImGui::Checkbox("Two-Sided Casters @ Low Sun",
                                state.sunShadowFrontFaceCullAtLowSun);
            }
            if (state.sunShadowFrontFaceCullAtLowSun != nullptr &&
                *state.sunShadowFrontFaceCullAtLowSun &&
                state.sunShadowFrontFaceCullGrazingThreshold != nullptr) {
                ImGui::SliderFloat("Two-Sided Start (grazing)",
                                   state.sunShadowFrontFaceCullGrazingThreshold, 0.50f, 0.98f,
                                   "%.2f");
            }

            if (ImGui::Button("Reset Shadow Bias")) {
                *state.sunShadowDirectionalBias = glm::vec3(0.000670f, 0.000115f, 0.0f);
                if (state.sunShadowLowSunBiasBoost != nullptr) {
                    *state.sunShadowLowSunBiasBoost = 1.8f;
                }
                if (state.sunShadowFrontFaceCullAtLowSun != nullptr) {
                    *state.sunShadowFrontFaceCullAtLowSun = true;
                }
                if (state.sunShadowFrontFaceCullGrazingThreshold != nullptr) {
                    *state.sunShadowFrontFaceCullGrazingThreshold = 0.78f;
                }
            }
        } else {
            ImGui::TextUnformatted("Shadow bias controls unavailable.");
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("TEMP: Gun Viewmodel Tuning")) {
        ImGui::TextUnformatted("Runtime-only tuning for first-person gun transform.");

        if (state.gunViewOffset != nullptr) {
            ImGui::DragFloat3("Gun Offset", &state.gunViewOffset->x, 0.005f, -2.0f, 2.0f, "%.3f");
        }
        if (state.gunViewScale != nullptr) {
            ImGui::DragFloat3("Gun Scale", &state.gunViewScale->x, 0.001f, 0.001f, 2.0f, "%.3f");
        }
        if (state.gunViewEulerDeg != nullptr) {
            ImGui::SliderFloat3("Gun Euler (deg)", &state.gunViewEulerDeg->x, -180.0f, 180.0f,
                                "%.1f");
        }

        if (state.gunViewOffset != nullptr && state.gunViewScale != nullptr &&
            state.gunViewEulerDeg != nullptr && ImGui::Button("Reset Gun Transform")) {
            *state.gunViewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
            *state.gunViewScale = glm::vec3(0.10f);
            *state.gunViewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
        }
    }

    ImGui::Separator();
    ImGui::Text("Network");
    ImGui::Text("Connected: %s", data.netConnected ? "yes" : "no");
    ImGui::Text("Status: %.*s", static_cast<int>(data.netStatus.size()),
                data.netStatus.data() != nullptr ? data.netStatus.data() : "");
    ImGui::Text("Server tick: %u | Acked input tick: %u", data.serverTick, data.ackedInputTick);
    ImGui::Text("Pending inputs: %zu", data.pendingInputCount);
    ImGui::Text("Chunk queues data/delta/unload: %zu / %zu / %zu", data.chunkDataQueueDepth,
                data.chunkDeltaQueueDepth, data.chunkUnloadQueueDepth);

    ImGui::Separator();
    if (state.cursorEnabled != nullptr) {
        ImGui::Checkbox("Cursor Enabled", state.cursorEnabled);
    }
    if (state.rawMouseInputEnabled != nullptr) {
        if (!state.rawMouseInputSupported) {
            ImGui::BeginDisabled();
            ImGui::Checkbox("Raw Mouse Input", state.rawMouseInputEnabled);
            ImGui::EndDisabled();
            ImGui::TextUnformatted("Raw mouse input not supported on this platform.");
        } else {
            ImGui::Checkbox("Raw Mouse Input", state.rawMouseInputEnabled);
        }
    }

    ImGui::Checkbox("Crosshair", &m_crosshairEnabled);
    ImGui::Checkbox("ImGui Demo Window", &m_showDemoWindow);

    ImGui::End();

    if (m_showDemoWindow) {
        ImGui::ShowDemoWindow(&m_showDemoWindow);
    }
}

void DebugUi::render() {
    if (!m_initialized) {
        return;
    }
    ImGui::Render();
#if VOXELOPS_IMGUI_OPENGL_BACKEND_AVAILABLE
    if (m_backendType == BackendType::OpenGL) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
#endif
}

void DebugUi::setVisible(bool visible) noexcept {
    m_visible = visible;
}

void DebugUi::toggleVisible() noexcept {
    m_visible = !m_visible;
}

bool DebugUi::isVisible() const noexcept {
    return m_visible;
}
