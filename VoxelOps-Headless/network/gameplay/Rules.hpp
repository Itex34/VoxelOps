#pragma once

#include "../../game/player/Hitbox.hpp"

#include <cstdint>

namespace Rules {

    float ToModelYawDegrees(float lookYawDegrees);
    float HeadshotDamageForWeapon(uint16_t weaponId);
    float TorsoshotDamageForWeapon(uint16_t weaponId);
    float LegshotDamageForWeapon(uint16_t weaponId);
    float MinSecondsPerShotForWeapon(uint16_t weaponId, float defaultSeconds);
    const char *HitRegionName(HitRegion region);
    HitRegion RegionFromCacheCode(uint8_t code);

} // namespace Rules
