#pragma once

#include <GLFW/glfw3.h>

class App {
public:
    App() = default;

    int Run();
    void Exit();

private:
    struct Runtime;

    bool initWindowAndContext();
    void initCallbacks(Runtime& runtime);
    void initRenderResources(Runtime& runtime);
    void initGameplay(Runtime& runtime);
    void configureBackendPolicy(Runtime& runtime);
    void initNetworking(Runtime& runtime);
    void processFrame(Runtime& runtime);
    void shutdown(Runtime& runtime);

    void updateDebugCamera(Runtime& runtime);
    void updateToggleStates();
    void processWorldInteraction(Runtime& runtime);
    void processNetworking(Runtime& runtime);

    void updateFPSCounter();

    GLFWwindow* m_Window = nullptr;
    bool m_UseDebugCamera = false;

    bool m_ToggleWireframe = false;
    bool m_ToggleChunkBorders = false;
    bool m_ToggleDebugFrustum = false;

    bool m_WasF1Pressed = false;
    bool m_WasTPressed = false;
    bool m_WasF2Pressed = false;
    bool m_WasF3Pressed = false;
};
