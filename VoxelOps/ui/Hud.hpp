#pragma once

#include "../runtime/Runtime.hpp"

class Hud {
public:
    Hud() = default;
    ~Hud() = default;
    void draw(Runtime &runtime);

private:
    void drawImGui(Runtime &runtime);


    void drawNative(Runtime &runtime);
};
