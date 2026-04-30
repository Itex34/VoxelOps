#pragma once

#include <cstdint>
#include <string>
#include <SDL3/SDL.h>
#include <glm/vec3.hpp>
#include "../runtime/Runtime.hpp"
#include "../graphics/RenderDeviceFactory.hpp"
#include "../graphics/OpenGL/WorldItemRenderer.hpp"
class Camera;
enum class GunType : uint16_t;
class FrameOrchestrator;

class App {
  public:
    App() = default;

    int Run(int argc, char **argv);
    void Exit();

  private:
    friend class FrameOrchestrator;
    bool initWindowAndContext();
    void initCallbacks(Runtime &runtime);
    void initRenderResources(Runtime &runtime);
    void initUi(Runtime &runtime);
    void initGameplay(Runtime &runtime);
    void preloadGuns(Runtime &runtime);
    void configureBackendPolicy(Runtime &runtime);
    void initNetworking(Runtime &runtime);
    bool beginConnectionAttempt(Runtime &runtime);
    void processFrame(Runtime &runtime);
    void shutdown(Runtime &runtime);

    void updateDebugCamera(Runtime &runtime);
    void updateToggleStates(Runtime &runtime);
    void processWorldInteraction(Runtime &runtime);
    void processShooting(Runtime &runtime);
    void processChunkStreaming(Runtime &runtime, bool prioritizeMovement);
    void renderWorldItems(Runtime &runtime, const Camera &activeCamera);
    bool equipGun(Runtime &runtime, GunType gunType);
    void applyMouseInputModes();
    void pollEvents(Runtime &runtime);

    void updateFPSCounter();
    void toggleFullscreen(SDL_Window *window);

    SDL_Window *m_Window = nullptr;
    SDL_GLContext m_GlContext = nullptr;
    RenderApi m_RenderApi = RenderApi::OpenGL;
    bool m_ShouldQuit = false;
    bool m_UseDebugCamera = false;

    bool m_IsFullscreen = false;
    bool m_ToggleWireframe = false;
    bool m_ToggleChunkBorders = false;
    bool m_ToggleDebugFrustum = false;
    bool m_ShowDebugUi = false;
    bool m_ShowInventoryUi = false;
    bool m_ForceCursorEnabled = false;
    bool m_EnableRawMouseInput = true;
    float m_SkyExposure = 4.2f;
    glm::vec3 m_SunDirection = glm::vec3(0.0f, 0.43496552f, 0.90044713f);
    glm::vec3 m_SunShadowDirectionalBias =
        glm::vec3(0.00067f, 0.000115f, 0.0f); // x=+Y, y=side, z=-Y
    float m_SunShadowLowSunBiasBoost = 5.8f;
    bool m_SunShadowFrontFaceCullAtLowSun = true;
    float m_SunShadowFrontFaceCullGrazingThreshold = 0.78f;
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
};
