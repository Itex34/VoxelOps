#include "UiStateController.hpp"

#include "../../network/ClientNetwork.hpp"

void UiStateController::update(RuntimeUiState &uiState, const ClientNetwork &network) const {
    if (network.IsConnected()) {
        uiState.activeView = UiView::InGame;
    } else {
        uiState.activeView = UiView::MainMenu;
        uiState.pauseMenuVisible = false;
        uiState.pauseMenuSettingsVisible = false;
    }
}
