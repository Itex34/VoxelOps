#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>

#include "../data/GameData.hpp"

class InputCallbacks {
public:
	InputCallbacks() = default;

	void framebuffer_size_callback(SDL_Window* window, int width, int height);
	void mouse_callback(SDL_Window* window, float xpos, float ypos, bool dbgCam);
	void mouse_button_callback(SDL_Window* window, uint8_t button, bool pressed);
	void processInput(SDL_Window* window);
};
