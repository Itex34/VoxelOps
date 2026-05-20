#include "UiStateController.hpp"

#include "../../network/ClientNetwork.hpp"

void UiStateController::update(RuntimeUiState &uiState, const ClientNetwork &network) const {
    if (network.IsConnected()) {
        uiState.activeView = UiView::InGame;
        uiState.wantsCursor = false;
    } else {
        uiState.activeView = UiView::MainMenu;
        uiState.wantsCursor = true;
    }
}
