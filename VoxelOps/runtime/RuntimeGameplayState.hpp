#pragma once

#include "../input/InputCallbacks.hpp"
#include "../physics/RayManager.hpp"
#include "../player/Player.hpp"
#include "../world/ChunkManager.hpp"

#include <memory>

struct RuntimeGameplayState {
    std::unique_ptr<ChunkManager> chunkManager;
    std::unique_ptr<Player> player;
    std::unique_ptr<InputCallbacks> inputCallbacks;

    RayManager rayManager;
};
