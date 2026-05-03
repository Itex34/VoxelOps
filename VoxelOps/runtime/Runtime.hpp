#pragma once

#include "../runtime/RuntimeAppState.hpp"
#include "../runtime/RuntimeCombatState.hpp"
#include "../runtime/RuntimeGameplayState.hpp"
#include "../runtime/RuntimeNetworkState.hpp"
#include "../runtime/RuntimePredictionState.hpp"
#include "../runtime/RuntimeRenderState.hpp"
#include "../runtime/RuntimeUiState.hpp"
#include "../runtime/RuntimeWorldState.hpp"

#include <cstdint>
#include <string>

struct Runtime {
    RuntimeGameplayState gameplay;
    RuntimeNetworkState network;

    RuntimeRenderState render;
    RuntimeWorldState world;
    RuntimePredictionState prediction;
    RuntimeUiState ui;
    RuntimeAppState app;
    RuntimeCombatState combat;
};
