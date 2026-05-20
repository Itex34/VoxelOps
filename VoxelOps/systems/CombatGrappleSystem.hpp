#pragma once

#include "../runtime/Runtime.hpp"

struct CombatGrappleSystemContext {
    bool useDebugCamera = false;
};

class CombatGrappleSystem {
public:
    void update(Runtime &runtime, const CombatGrappleSystemContext &ctx);
};
