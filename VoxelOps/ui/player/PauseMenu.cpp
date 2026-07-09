#include "PauseMenu.hpp"

#include "../../application/FrameServices.hpp"
#include "../../data/GameData.hpp"
#include "../../runtime/Runtime.hpp"
#include "../widgets/UIContext.hpp"

#include <algorithm>

void PauseMenu::draw(Runtime &runtime, FrameConnectionHost *connectionHost, FrameWindowHost *windowHost) {
    if (!runtime.ui.pauseMenuVisible || runtime.ui.activeView != UiView::InGame) {
        return;
    }
    if (!runtime.ui.nativeUi || !runtime.ui.nativeUi->hasBackendRenderer()) {
        return;
    }

    GameData::cursorEnabled = true;

    UIContext &ui = runtime.ui.nativeUi->context();
    const glm::vec2 screen = ui.screenSize();

    ui.panel(Rect{0.0f, 0.0f, screen.x, screen.y}, Color{0.0f, 0.0f, 0.0f, 0.46f});

    const float panelWidth = std::min(360.0f, std::max(300.0f, screen.x - 48.0f));
    const float panelHeight = runtime.ui.pauseMenuSettingsVisible ? 260.0f : 236.0f;
    const float x = (screen.x - panelWidth) * 0.5f;
    const float y = (screen.y - panelHeight) * 0.5f;

    ui.panel(Rect{x, y, panelWidth, panelHeight}, Color{0.035f, 0.038f, 0.043f, 0.96f});
    ui.panel(Rect{x, y, panelWidth, 2.0f}, Color{1.0f, 0.63f, 0.22f, 1.0f});
    ui.labelInRect(
        runtime.ui.pauseMenuSettingsVisible ? "Settings" : "Paused",
        Rect{x + 24.0f, y + 20.0f, panelWidth - 48.0f, 24.0f},
        Color{0.98f, 0.98f, 0.98f, 1.0f},
        TextAlign::Center,
        TextVerticalAlign::Center
    );

    const float buttonX = x + 42.0f;
    const float buttonW = panelWidth - 84.0f;
    const float buttonH = 38.0f;
    const float firstY = y + 72.0f;
    const float gap = 14.0f;

    if (ui.button("Resume", Rect{buttonX, firstY, buttonW, buttonH})) {
        runtime.ui.pauseMenuVisible = false;
        runtime.ui.pauseMenuSettingsVisible = false;
        //runtime.ui.wantsCursor = false;
    }
    if (ui.button("Settings", Rect{buttonX, firstY + buttonH + gap, buttonW, buttonH})) {
        runtime.ui.pauseMenuVisible = false;

        runtime.ui.pauseMenuSettingsVisible = true;
    }
    if (ui.button("Leave Game", Rect{buttonX, firstY + (buttonH + gap) * 2.0f, buttonW, buttonH})) {
        if (connectionHost != nullptr) {
            connectionHost->leaveGame(runtime);
        }
    }

    if (windowHost != nullptr) {
        windowHost->applyMouseInputModes();
    }
}
