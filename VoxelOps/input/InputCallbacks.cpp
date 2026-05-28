#include "InputCallbacks.hpp"
#include <glad/glad.h>

InputCallbacks::InputCallbacks(Player &inPlayer)
    : player(inPlayer) {}

void InputCallbacks::framebuffer_size_callback(SDL_Window *window, int width, int height) {
    (void)window;
    GameData::screenWidth = width;
    GameData::screenHeight = height;
    if (SDL_GL_GetCurrentContext() != nullptr) {
        glViewport(0, 0, width, height);
    }
}

void InputCallbacks::mouse_motion_callback(
    SDL_Window *window, float xpos, float ypos, float xrel, float yrel, bool dbgCam
) {
    if (GameData::cursorEnabled)
        return;
    const bool relativeMouseMode = (window != nullptr) && SDL_GetWindowRelativeMouseMode(window);
    // When gameplay has locked relative mouse mode, transient UI capture flags should not
    // suppress camera look.
    if (GameData::uiWantsMouseCapture && !relativeMouseMode)
        return;
    if (relativeMouseMode) {
        m_virtualMouseX += static_cast<double>(xrel);
        m_virtualMouseY += static_cast<double>(yrel);
    } else {
        m_virtualMouseX = static_cast<double>(xpos);
        m_virtualMouseY = static_cast<double>(ypos);
    }
    player.processMouse(dbgCam, m_virtualMouseX, m_virtualMouseY);
}

void InputCallbacks::mouse_button_callback(SDL_Window *window, uint8_t button, bool pressed) {
    (void)window;
    if (GameData::uiWantsMouseCapture)
        return;
    if (button == SDL_BUTTON_LEFT && pressed) {
        GameData::cursorEnabled = false;
    }
}

void InputCallbacks::processInput(SDL_Window *window) {
    (void)window;
    if (GameData::uiWantsKeyboardCapture || GameData::uiWantsTextInput)
        return;
    int keyCount = 0;
    const bool *keys = SDL_GetKeyboardState(&keyCount);
    if (keys && SDL_SCANCODE_ESCAPE < keyCount && keys[SDL_SCANCODE_ESCAPE]) {
        GameData::cursorEnabled = true;
    }
}
