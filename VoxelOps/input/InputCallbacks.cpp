#include "InputCallbacks.hpp"


InputCallbacks::InputCallbacks(Player& inPlayer) : player(inPlayer) {}

void InputCallbacks::framebuffer_size_callback(GLFWwindow* window, int width, int height) {
	GameData::screenWidth = width;
	GameData::screenHeight = height;
	glViewport(0, 0, width, height);
}

void InputCallbacks::mouse_callback(GLFWwindow* window, double xpos, double ypos, bool dbgCam) {
	if (GameData::cursorEnabled) return;
	player.processMouse( dbgCam, xpos, ypos);
}

void InputCallbacks::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
	if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		GameData::cursorEnabled = false;
	}
}

void InputCallbacks::processInput(GLFWwindow* window) {
	double currentFrame = glfwGetTime();
	GameData::deltaTime = currentFrame - GameData::lastFrame;
	GameData::lastFrame = currentFrame;

	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
		GameData::cursorEnabled = true;
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	}
}
