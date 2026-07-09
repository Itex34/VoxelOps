#pragma once

struct Runtime;
struct FrameConnectionHost;
struct FrameWindowHost;

class PauseMenu {
public:
    void draw(Runtime &runtime, FrameConnectionHost *connectionHost, FrameWindowHost *windowHost);
};
