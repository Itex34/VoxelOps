#pragma once

#include "../runtime/Runtime.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>

struct ClientHudSystemContext {
    SDL_Window *window = nullptr;
    std::string *serverIp = nullptr;
    uint16_t *serverPort = nullptr;
    std::string *requestedUsername = nullptr;
    std::function<bool(Runtime &)> beginConnectionAttempt;
    std::function<void()> applyMouseInputModes;
};

class ClientHudSystem {
public:
    void draw(Runtime &runtime, const ClientHudSystemContext &ctx);

private:
    void drawConnectionPrompt(Runtime &runtime, const ClientHudSystemContext &ctx);
    void drawKillFeed(Runtime &runtime);
    void drawScoreboard(Runtime &runtime);
    void drawPingCounter(Runtime &runtime);
    void drawPlayerHud(Runtime &runtime);
    void drawDeathOverlay(Runtime &runtime);
};
