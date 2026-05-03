#pragma once

#include "../player/Player.hpp"

struct ClientInputIntent {
    NetworkInputState networkInput{};
    bool gameplayInputEnabled = false;
};
