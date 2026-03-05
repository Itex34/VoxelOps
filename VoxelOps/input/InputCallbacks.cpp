#include "InputCallbacks.hpp"
#include <imgui.h>


InputCallbacks::InputCallbacks(Player& inPlayer) : player(inPlayer) {}

void InputCallbacks::framebuffer_size_callback(GLFWwindow* window, int width, int height) {
	GameData::screenWidth = width;
	GameData::screenHeight = height;
	glViewport(0, 0, width, height);
}

void InputCallbacks::mouse_callback(GLFWwindow* window, double xpos, double ypos, bool dbgCam) {
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) return;
	if (GameData::cursorEnabled) return;
	player.processMouse( dbgCam, xpos, ypos);
}

void InputCallbacks::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    (void)window;
    (void)mods;
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) return;
	if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
		GameData::cursorEnabled = false;
	}
}

void InputCallbacks::processInput(GLFWwindow* window) {
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard) return;
	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
		GameData::cursorEnabled = true;
	}
}
