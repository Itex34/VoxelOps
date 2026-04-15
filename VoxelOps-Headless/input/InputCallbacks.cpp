#include "InputCallbacks.hpp"


void InputCallbacks::framebuffer_size_callback(SDL_Window* window, int width, int height) {
	GameData::screenWidth = width;
	GameData::screenHeight = height;
	glViewport(0, 0, width, height);
	(void)window;
}

void InputCallbacks::mouse_callback(SDL_Window* window, float xpos, float ypos, bool dbgCam) {
	if (GameData::cursorEnabled) return;
	(void)window;
	(void)xpos;
	(void)ypos;
	(void)dbgCam;
}

void InputCallbacks::mouse_button_callback(SDL_Window* window, uint8_t button, bool pressed) {
	if (button == SDL_BUTTON_LEFT && pressed && window != nullptr) {
		SDL_SetWindowRelativeMouseMode(window, true);
		GameData::cursorEnabled = false;
	}
	(void)window;
}

void InputCallbacks::processInput(SDL_Window* window) {
	const double currentFrame = static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0;
	GameData::deltaTime = currentFrame - GameData::lastFrame;
	GameData::lastFrame = currentFrame;

	int keyCount = 0;
	const bool* keys = SDL_GetKeyboardState(&keyCount);
	const bool escapePressed = keys != nullptr && SDL_SCANCODE_ESCAPE < keyCount && keys[SDL_SCANCODE_ESCAPE];
	if (escapePressed) {
		GameData::cursorEnabled = true;
		if (window != nullptr) {
			SDL_SetWindowRelativeMouseMode(window, false);
		}
	}
}
