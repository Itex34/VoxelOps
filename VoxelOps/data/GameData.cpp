#include "GameData.hpp"

#pragma once
#include <glm/glm.hpp>


namespace GameData {

	int screenWidth = 800, screenHeight = 600;

	double deltaTime = 0.0f;
	double lastFrame = 0.0f;
	double fpsTime = 0.0f;
	int frameCount = 0;

	float lastX = 400, lastY = 300;
	float farPlane = 1000.0f;
	float nearPlane = 0.1f;
	float FOV = 80.0f;

	glm::vec3 startPos = glm::vec3(0.0f, 20.0f, 0.0f);

	bool firstMouse = true;
	bool cursorEnabled = false;


	float xPos = 0.0f;
	float yPos = 0.0f;
	float zPos = 0.0f;

	glm::vec3 pistolPos = glm::vec3(xPos, yPos, zPos);
}