#pragma once

#include "../player/Hitbox.hpp"

#include <cstdint>

namespace CombatRules {

float ToModelYawDegrees(float lookYawDegrees);
float HeadshotDamageForWeapon(uint16_t weaponId);
float TorsoshotDamageForWeapon(uint16_t weaponId);
float LegshotDamageForWeapon(uint16_t weaponId);
float MinSecondsPerShotForWeapon(uint16_t weaponId, float defaultSeconds);
const char* HitRegionName(HitRegion region);
HitRegion RegionFromCacheCode(uint8_t code);

} // namespace CombatRules

