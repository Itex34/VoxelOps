#pragma once
#include <glm/glm.hpp>


namespace GameData {

	extern int screenWidth, screenHeight;

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


	extern float xPos;
	extern float yPos;
	extern float zPos;

	extern glm::vec3 pistolPos;
}