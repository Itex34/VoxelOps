#pragma once

#include "../runtime/Runtime.hpp"

class Hud {
public:
    void draw(Runtime &runtime);

private:
    void drawKillFeed(Runtime &runtime);
    void drawScoreboard(Runtime &runtime);
    void drawPingCounter(Runtime &runtime);
    void drawPlayerHud(Runtime &runtime);
    void drawDeathOverlay(Runtime &runtime);
};
