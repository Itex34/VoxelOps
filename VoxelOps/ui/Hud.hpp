#pragma once

#include "../runtime/Runtime.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>

struct HudContext {
    SDL_Window *window = nullptr;
    std::string *serverIp = nullptr;
    uint16_t *serverPort = nullptr;
    std::string *requestedUsername = nullptr;
    std::function<bool(Runtime &)> beginConnectionAttempt;
    std::function<void()> applyMouseInputModes;
};

class Hud {
public:
    void draw(Runtime &runtime, const HudContext &ctx);

private:
    void drawConnectionPrompt(Runtime &runtime, const HudContext &ctx);
    void drawKillFeed(Runtime &runtime);
    void drawScoreboard(Runtime &runtime);
    void drawPingCounter(Runtime &runtime);
    void drawPlayerHud(Runtime &runtime);
    void drawDeathOverlay(Runtime &runtime);
};
