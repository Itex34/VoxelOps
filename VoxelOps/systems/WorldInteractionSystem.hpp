#pragma once

#include "../runtime/Runtime.hpp"

struct WorldInteractionSystemContext {
    bool *wasWorldInteractPressed = nullptr;
};

class WorldInteractionSystem {
public:
    void update(Runtime &runtime, const WorldInteractionSystemContext &ctx);
};
