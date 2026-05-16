#pragma once

#include <cstdint>

struct Runtime;
struct SDL_Window;
class Camera;
enum class GunType : uint16_t;
class App;

struct FrameInputHost {
    explicit FrameInputHost(App &app)
        : app(app) {}

    void updateDebugCamera(Runtime &runtime);
    void updateToggleStates(Runtime &runtime);
    void pollEvents(Runtime &runtime);

    App &app;
};

struct FrameConnectionHost {
    explicit FrameConnectionHost(App &app)
        : app(app) {}

    bool beginConnectionAttempt(Runtime &runtime);
    bool equipGun(Runtime &runtime, GunType gunType);

    App &app;
};

struct FrameWindowHost {
    explicit FrameWindowHost(App &app)
        : app(app) {}

    void applyMouseInputModes();
    void updateFPSCounter();
    void toggleFullscreen(SDL_Window *window);

    App &app;
};

struct FrameRenderHost {
    explicit FrameRenderHost(App &app)
        : app(app) {}

    void renderWorldItems(Runtime &runtime, const Camera &activeCamera);

    App &app;
};
