#pragma once


#include "graphics/Vulkan/scene/Camera.hpp"
#include <cstdint>

struct SDL_Window;


class Player {
public:
	Player(glm::vec3 startPos);
	void update(SDL_Window* window);
	const Camera& getCamera() const { return playerCamera; }
	Camera& getCamera() { return playerCamera; }


private:
	void processMouseInput(float deltaX, float deltaY) noexcept;
	void processKeyboardInput(SDL_Window* window, float deltaTimeSeconds);

	Camera playerCamera;
	float moveSpeed = 4.5f;
	float sprintMultiplier = 1.8f;
	float mouseSensitivity = 0.12f;
	uint64_t lastUpdateNs = 0;
	bool escapePressedLastFrame = false;
	bool mouseWasRelativeLastFrame = false;
};
