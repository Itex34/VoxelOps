#include "DebugUi.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

bool DebugUi::initialize(GLFWwindow* window, const char* glslVersion) {
    if (m_initialized) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_initialized = true;
    return true;
}

void DebugUi::shutdown() {
    if (!m_initialized) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void DebugUi::beginFrame() {
    if (!m_initialized) {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void DebugUi::drawCrosshair(bool enabled) {
    if (!m_initialized || !enabled || !m_crosshairEnabled) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    constexpr float kGap = 5.0f;
    constexpr float kArm = 8.0f;
    constexpr float kThickness = 2.0f;
    const ImU32 color = IM_COL32(245, 245, 245, 230);

    drawList->AddLine(
        ImVec2(center.x - (kGap + kArm), center.y),
        ImVec2(center.x - kGap, center.y),
        color, kThickness
    );
    drawList->AddLine(
        ImVec2(center.x + kGap, center.y),
        ImVec2(center.x + (kGap + kArm), center.y),
        color, kThickness
    );
    drawList->AddLine(
        ImVec2(center.x, center.y - (kGap + kArm)),
        ImVec2(center.x, center.y - kGap),
        color, kThickness
    );
    drawList->AddLine(
        ImVec2(center.x, center.y + kGap),
        ImVec2(center.x, center.y + (kGap + kArm)),
        color, kThickness
    );
}

void DebugUi::drawMainWindow(const UiFrameData& data, UiMutableState& state) {
    if (!m_initialized || !m_visible) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VoxelOps Debug", &m_visible)) {
        ImGui::End();
        return;
    }

    const std::string_view backendName = data.backendName.empty()
        ? std::string_view("unknown")
        : data.backendName;

    ImGui::Text("FPS: %.1f (%.2f ms)", data.fps, data.frameMs);
    ImGui::Text("Backend: %.*s | MDI: %s",
        static_cast<int>(backendName.size()),
        backendName.data(),
        data.mdiUsable ? "yes" : "no");

    ImGui::Separator();
    ImGui::Text("Player");
    ImGui::Text("Pos: (%.2f, %.2f, %.2f)", data.playerPosition.x, data.playerPosition.y, data.playerPosition.z);
    ImGui::Text("Vel: (%.2f, %.2f, %.2f)", data.playerVelocity.x, data.playerVelocity.y, data.playerVelocity.z);
    ImGui::Text("Fly mode: %s | Grounded: %s", data.flyMode ? "on" : "off", data.onGround ? "yes" : "no");
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
            ImGui::SliderFloat3("Gun Euler (deg)", &state.gunViewEulerDeg->x, -180.0f, 180.0f, "%.1f");
        }

        if (
            state.gunViewOffset != nullptr &&
            state.gunViewScale != nullptr &&
            state.gunViewEulerDeg != nullptr &&
            ImGui::Button("Reset Gun Transform")
        ) {
            *state.gunViewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
            *state.gunViewScale = glm::vec3(0.10f);
            *state.gunViewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
        }
    }

    ImGui::Separator();
    ImGui::Text("Network");
    ImGui::Text("Connected: %s", data.netConnected ? "yes" : "no");
    ImGui::Text("Status: %.*s",
        static_cast<int>(data.netStatus.size()),
        data.netStatus.data() != nullptr ? data.netStatus.data() : "");
    ImGui::Text("Server tick: %u | Acked input seq: %u", data.serverTick, data.ackedInputSeq);
    ImGui::Text("Pending inputs: %zu", data.pendingInputCount);
    ImGui::Text("Chunk queues data/delta/unload: %zu / %zu / %zu",
        data.chunkDataQueueDepth, data.chunkDeltaQueueDepth, data.chunkUnloadQueueDepth);

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
        }
        else {
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
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
