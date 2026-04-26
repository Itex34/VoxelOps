#pragma once

#include "../player/PlayerManager.hpp"
#include "../player/Hitbox.hpp"

#include <cstdint>

namespace Damage {

struct DamageResolution {
    bool applied = false;
    bool killed = false;
    float healthAfter = 0.0f;
    float damage = 0.0f;
};

float ComputeDamage(uint16_t weaponId, HitRegion region);

DamageResolution ResolveDamage(PlayerManager &playerManager,
                               PlayerID targetId,
                               uint16_t weaponId,
                               HitRegion region);

} // namespace Damage
