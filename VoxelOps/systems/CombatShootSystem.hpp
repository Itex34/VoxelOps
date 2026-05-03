#pragma once

#include "../runtime/Runtime.hpp"

struct CombatShootSystemContext {
    bool useDebugCamera = false;
};

class CombatShootSystem {
public:
    void update(Runtime &runtime, const CombatShootSystemContext &ctx);
};
