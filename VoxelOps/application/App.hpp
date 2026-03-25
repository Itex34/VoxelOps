#pragma once

#include <cstdint>
#include <string>
#include "../runtime/Runtime.hpp"
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
    void processChunkStreaming(Runtime& runtime, bool prioritizeMovement);
    void drawKillFeed(Runtime& runtime);
    void drawScoreboard(Runtime& runtime);
    void drawPingCounter(Runtime& runtime);
    void drawDeathOverlay(Runtime& runtime);
    bool equipGun(Runtime& runtime, GunType gunType);
    void renderRemotePlayerGuns(Runtime& runtime, const Camera& activeCamera);
    void renderHeldGun(Runtime& runtime, const Camera& activeCamera);
    void applyMouseInputModes();

    void updateFPSCounter();

    GLFWwindow* m_Window = nullptr;
    bool m_UseDebugCamera = false;

    bool m_ToggleWireframe = false;
    bool m_ToggleChunkBorders = false;
    bool m_ToggleDebugFrustum = false;
    bool m_ShowDebugUi = false;
    bool m_EnableRawMouseInput = true;

    std::string m_ServerIp = "variety-reduction.gl.at.ply.gg:20047";
    uint16_t m_ServerPort = 27015;
    std::string m_RequestedUsername;

    bool m_WasF1Pressed = false;
    bool m_WasTPressed = false;
    bool m_WasF2Pressed = false;
    bool m_WasF3Pressed = false;
    bool m_WasF10Pressed = false;
};
