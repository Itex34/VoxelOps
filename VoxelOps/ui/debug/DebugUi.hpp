#pragma once

#include <GLFW/glfw3.h>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

struct UiFrameData {
    float fps = 0.0f;
    float frameMs = 0.0f;
    glm::vec3 playerPosition{ 0.0f };
    glm::vec3 playerVelocity{ 0.0f };
    bool flyMode = false;
    bool onGround = false;
    uint16_t renderDistance = 0;
    size_t remotePlayerCount = 0;
    bool netConnected = false;
    std::string_view netStatus{};
    uint32_t serverTick = 0;
    uint32_t ackedInputSeq = 0;
    size_t pendingInputCount = 0;
    size_t chunkDataQueueDepth = 0;
    size_t chunkDeltaQueueDepth = 0;
    size_t chunkUnloadQueueDepth = 0;
    std::string_view backendName{};
    bool mdiUsable = false;
};

struct UiMutableState {
    bool* useDebugCamera = nullptr;
    bool* toggleWireframe = nullptr;
    bool* toggleChunkBorders = nullptr;
    bool* toggleDebugFrustum = nullptr;
    uint16_t* renderDistance = nullptr;
    bool* cursorEnabled = nullptr;
    bool* rawMouseInputEnabled = nullptr;
    bool rawMouseInputSupported = true;
    glm::vec3* gunViewOffset = nullptr;
    glm::vec3* gunViewScale = nullptr;
    glm::vec3* gunViewEulerDeg = nullptr;
};

class DebugUi {
public:
    bool initialize(GLFWwindow* window, const char* glslVersion);
    void shutdown();

    void beginFrame();
    void drawCrosshair(bool enabled);
    void drawMainWindow(const UiFrameData& data, UiMutableState& state);
    void render();

    void setVisible(bool visible) noexcept;
    void toggleVisible() noexcept;
    [[nodiscard]] bool isVisible() const noexcept;

private:
    bool m_initialized = false;
    bool m_visible = true;
    bool m_showDemoWindow = false;
    bool m_crosshairEnabled = true;
};
