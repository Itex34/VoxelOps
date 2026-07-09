#include "FrameServices.hpp"

#include "App.hpp"

void FrameInputHost::updateDebugCamera(Runtime &runtime) {
    app.updateDebugCamera(runtime);
}

void FrameInputHost::updateToggleStates(Runtime &runtime) {
    app.updateToggleStates(runtime);
}

void FrameInputHost::pollEvents(Runtime &runtime) {
    app.pollEvents(runtime);
}

bool FrameConnectionHost::beginConnectionAttempt(Runtime &runtime) {
    return app.beginConnectionAttempt(runtime);
}

void FrameConnectionHost::leaveGame(Runtime &runtime) {
    app.leaveGame(runtime);
}

bool FrameConnectionHost::equipGun(Runtime &runtime, GunType gunType) {
    return app.equipGun(runtime, gunType);
}

void FrameWindowHost::applyMouseInputModes() {
    app.applyMouseInputModes();
}

void FrameWindowHost::updateFPSCounter() {
    app.updateFPSCounter();
}

void FrameWindowHost::toggleFullscreen(SDL_Window *window) {
    app.toggleFullscreen(window);
}

void FrameRenderHost::renderWorldItems(Runtime &runtime, const Camera &activeCamera) {
    app.renderWorldItems(runtime, activeCamera);
}
