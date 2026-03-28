#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "../runtime/Runtime.hpp"
#include "WorldItemRenderer.hpp"
class Camera;
struct GLFWwindow;
enum class GunType : uint16_t;

class App {
public:
    App() = default;

    int Run(int argc, char** argv);
    void Exit();

private:

    bool initWindowAndContext();
    void initCallbacks(Runtime& runtime);
    void initRenderResources(Runtime& runtime);
    void initUi(Runtime& runtime);
    void initGameplay(Runtime& runtime);
    void preloadGuns(Runtime& runtime);
    void configureBackendPolicy(Runtime& runtime);
    void initNetworking(Runtime& runtime);
    bool beginConnectionAttempt(Runtime& runtime);
    void drawConnectionPrompt(Runtime& runtime);
    void processFrame(Runtime& runtime);
    void shutdown(Runtime& runtime);

    void updateDebugCamera(Runtime& runtime);
    void updateToggleStates(Runtime& runtime);
    void processWorldInteraction(Runtime& runtime);
    void processShooting(Runtime& runtime);
    void processMovementNetworking(Runtime& runtime);
    void processHotbarSelection(Runtime& runtime);
    void syncEquippedGunFromInventory(Runtime& runtime);
    void processChunkStreaming(Runtime& runtime, bool prioritizeMovement);
    void drawKillFeed(Runtime& runtime);
    void drawScoreboard(Runtime& runtime);
    void drawPingCounter(Runtime& runtime);
    void drawPlayerHud(Runtime& runtime);
    void drawDeathOverlay(Runtime& runtime);
    void renderWorldItems(Runtime& runtime, const Camera& activeCamera);
    bool equipGun(Runtime& runtime, GunType gunType);
    void renderRemotePlayerGuns(Runtime& runtime, const Camera& activeCamera);
    void renderHeldGun(Runtime& runtime, const Camera& activeCamera);
    void applyMouseInputModes();

    void updateFPSCounter();
    void toggleFullscreen(GLFWwindow* window);
    
    GLFWwindow* m_Window = nullptr;
    bool m_UseDebugCamera = false;

    bool m_IsFullscreen = false;
    bool m_ToggleWireframe = false;
    bool m_ToggleChunkBorders = false;
    bool m_ToggleDebugFrustum = false;
    bool m_ShowDebugUi = false;
    bool m_ShowInventoryUi = false;
    bool m_ForceCursorEnabled = false;
    bool m_EnableRawMouseInput = true;
    WorldItemRenderer m_worldItemRenderer;

    std::string m_ServerIp = "variety-reduction.gl.at.ply.gg:20047";
    uint16_t m_ServerPort = 27015;
    std::string m_RequestedUsername;

    bool m_WasF1Pressed = false;
    bool m_WasTPressed = false;
    bool m_WasF2Pressed = false;
    bool m_WasF3Pressed = false;
    bool m_WasXPressed = false;
    bool m_WasEscapePressed = false;
    bool m_WasF10Pressed = false;
    bool m_WasWorldInteractPressed = false;
    std::array<bool, kHotbarSlots> m_WasHotbarSelectPressed{};
};
