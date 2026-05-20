#pragma once

#include "../../runtime/RuntimeUiState.hpp"

class ClientNetwork;

class UiStateController {
public:
    void update(RuntimeUiState &uiState, const ClientNetwork &network) const;
};
