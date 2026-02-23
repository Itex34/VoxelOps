#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "../data/GameData.hpp"

class InputCallbacks {
public:
	InputCallbacks() = default;

	void framebuffer_size_callback(GLFWwindow* window, int width, int height);
	void mouse_callback(GLFWwindow* window, double xpos, double ypos, bool dbgCam);
	void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
	void processInput(GLFWwindow* window);
};
