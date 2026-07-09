#pragma once

struct Runtime;
struct FrameConnectionHost;
struct FrameWindowHost;

class SettingsMenu {
public:
    void draw(Runtime &runtime, FrameConnectionHost *connectionHost, FrameWindowHost *windowHost);
};
