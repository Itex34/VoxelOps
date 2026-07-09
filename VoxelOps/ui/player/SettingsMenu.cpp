#include "SettingsMenu.hpp"

#include "../../application/FrameServices.hpp"
#include "../../data/GameData.hpp"
#include "../../runtime/Runtime.hpp"
#include "../widgets/UIContext.hpp"

#include <algorithm>

void SettingsMenu::draw(
    Runtime& runtime, FrameConnectionHost* connectionHost, FrameWindowHost* windowHost
) {
    if (!runtime.ui.pauseMenuSettingsVisible || runtime.ui.activeView != UiView::InGame) {
        return;
    }
    if (!runtime.ui.nativeUi || !runtime.ui.nativeUi->hasBackendRenderer()) {
        return;
    }

    GameData::cursorEnabled = true;

    UIContext &ui = runtime.ui.nativeUi->context();
    const glm::vec2 screenSize = ui.screenSize();

    ui.panel(Rect{0.0f, 0.0f, screenSize.x, screenSize.y}, Color{0.0f, 0.0f, 0.0f, 0.46f});

    const float panelWidth = std::min(360.0f, std::max(300.0f, screenSize.x - 48.0f));
    const float panelHeight = runtime.ui.pauseMenuSettingsVisible ? 260.0f : 236.0f;
    const float x = (screenSize.x - panelWidth) * 0.5f;
    const float y = (screenSize.y - panelHeight) * 0.5f;
         
    ui.panel(Rect{x, y, panelWidth, panelHeight}, Color{0.035f, 0.038f, 0.043f, 0.96f});
    ui.panel(Rect{x, y, panelWidth, 2.0f}, Color{1.0f, 0.63f, 0.22f, 1.0f});


    const float buttonX = x + 42.0f;
    const float buttonW = panelWidth - 84.0f;
    const float buttonH = 38.0f;
    const float firstY = y + 72.0f;
    const float gap = 14.0f;


    if (ui.button("Back", Rect{buttonX, firstY + (buttonH + gap) * 2.0f, buttonW, buttonH})) {
        runtime.ui.pauseMenuSettingsVisible = false;

        runtime.ui.pauseMenuVisible = false;
    }

    if (windowHost != nullptr) {
        windowHost->applyMouseInputModes();
    }

}
