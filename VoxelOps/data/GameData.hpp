#pragma once
#include <glm/glm.hpp>

namespace GameData {

    extern int screenWidth, screenHeight;
    extern int windowedX, windowedY;

    extern double deltaTime;
    extern double lastFrame;
    extern double fpsTime;
    extern int frameCount;

    extern float lastX, lastY;
    extern float farPlane;
    extern float nearPlane;
    extern float FOV;

    extern glm::vec3 startPos;

    extern bool firstMouse;
    extern bool cursorEnabled;
    extern bool gameplayInputEnabled;
    extern bool uiWantsMouseCapture;
    extern bool uiWantsKeyboardCapture;
    extern bool uiWantsTextInput;
    // 0 = Auto, 1 = Software DDA, 2 = Hardware RT
    extern int giTracingBackendPreference;
    // NRD debug mode index from Debug UI combo (0..29)
    extern int giNrdDebugView;
    // 0 = Off, 1 = Flat Normal+Roughness, 2 = Flat Normal+Roughness + Zero Motion
    extern int giNrdGuideOverride;

    extern float xPos;
    extern float yPos;
    extern float zPos;

    extern glm::vec3 pistolPos;
} // namespace GameData
