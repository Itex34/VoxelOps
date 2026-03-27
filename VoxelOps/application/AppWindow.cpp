#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "App.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>

void App::updateFPSCounter() {
    GameData::frameCount++;
    const double currentTime = glfwGetTime();
    const double elapsedTime = currentTime - GameData::fpsTime;

    if (elapsedTime >= 2.0) {
        const double fps = GameData::frameCount / elapsedTime;
        std::stringstream ss;
        ss << "Voxel Ops - FPS: " << fps;
        glfwSetWindowTitle(m_Window, ss.str().c_str());

        GameData::frameCount = 0;
        GameData::fpsTime = currentTime;
    }
}


void App::applyMouseInputModes() {
    if (!m_Window) {
        return;
    }

    const bool cursorEnabled = GameData::cursorEnabled;
    glfwSetInputMode(m_Window, GLFW_CURSOR, cursorEnabled ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);

    if (glfwRawMouseMotionSupported()) {
        const int rawEnabled = (!cursorEnabled && m_EnableRawMouseInput) ? GLFW_TRUE : GLFW_FALSE;
        glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, rawEnabled);
    }
}


void App::Exit() {
    if (m_Window) {
        glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    }
}


bool App::initWindowAndContext() {
    if (!glfwInit()) {
        return false;
    }

    auto createWindowForVersion = [](int major, int minor) -> GLFWwindow* {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        return glfwCreateWindow(GameData::screenWidth, GameData::screenHeight, "Voxel Ops", nullptr, nullptr);
    };

    m_Window = createWindowForVersion(4, 3);
    if (!m_Window) {
        std::cerr << "OpenGL 4.3 context creation failed, retrying with OpenGL 3.3.\n";
        m_Window = createWindowForVersion(3, 3);
    }
    if (!m_Window) {
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_Window);
    // Default to VSync for stable frame pacing and smoother perceived camera motion.
    glfwSwapInterval(0);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD.\n";
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
        glfwTerminate();
        return false;
    }

    printf("OpenGL version: %s\n", glGetString(GL_VERSION));
    printf("GLSL version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    return true;
}


void App::toggleFullscreen(GLFWwindow* window)
{
    m_IsFullscreen = !m_IsFullscreen;

    if (m_IsFullscreen)
    {
        // Save current window position/size
        glfwGetWindowPos(window, &GameData::windowedX, &GameData::windowedY);
        glfwGetWindowSize(window, &GameData::screenWidth, &GameData::screenHeight);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        glfwSetWindowMonitor(window, monitor,
            0, 0,
            mode->width, mode->height,
            mode->refreshRate
        );
    }
    else
    {
        // Restore windowed mode
        glfwSetWindowMonitor(window, NULL,
            GameData::windowedX, GameData::windowedY,
            GameData::screenWidth, GameData::screenHeight,
            0
        );
    }
}

