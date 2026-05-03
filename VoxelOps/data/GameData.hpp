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
    // 0 = Auto, 1 = Software DDA, 2 = Hardware RT
    extern int giTracingBackendPreference;
    // 0 = Off, 1 = Diff Radiance, 2 = Hit Distance, 3 = Normal, 4 = Motion, 5 = ViewZ
    extern int giNrdDebugView;

    extern float xPos;
    extern float yPos;
    extern float zPos;

    extern glm::vec3 pistolPos;
} // namespace GameData
