#pragma once

#include "../../runtime/Runtime.hpp"
#include "../../application/FrameServices.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

struct MainMenuContext {
    SDL_Window *window = nullptr;
    std::string *serverIp = nullptr;
    uint16_t *serverPort = nullptr;
    std::string *requestedUsername = nullptr;
    FrameConnectionHost *connectionHost = nullptr;
    FrameWindowHost *windowHost = nullptr;
};

class MainMenu {
public:
    void draw(Runtime &runtime, const MainMenuContext &ctx);
};
