#pragma once

#include <SDL3/SDL.h>

#include "../data/GameData.hpp"
#include "../player/Player.hpp"

class InputCallbacks {
public:
    InputCallbacks(Player &inPlayer);

    void framebuffer_size_callback(SDL_Window *window, int width, int height);
    void mouse_motion_callback(
        SDL_Window *window, float xpos, float ypos, float xrel, float yrel, bool dbgCam
    );
    void mouse_button_callback(SDL_Window *window, uint8_t button, bool pressed);
    void processInput(SDL_Window *window);

private:
    Player &player;
    double m_virtualMouseX = 0.0;
    double m_virtualMouseY = 0.0;
};
