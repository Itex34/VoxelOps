#include "Rules.hpp"

#include "../../../Shared/gun/GunType.hpp"

#include <cmath>

namespace {

constexpr bool kPlayerModelYawInvert = true;
constexpr float kPlayerModelYawOffsetDeg = 0.0f;

float NormalizeYawDegrees(float yawDegrees) {
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f)
        y -= 360.0f;
    if (y < -180.0f)
        y += 360.0f;
    return y;
}

} // namespace

namespace CombatRules {

float ToModelYawDegrees(float lookYawDegrees) {
    const float signedYaw = kPlayerModelYawInvert ? -lookYawDegrees : lookYawDegrees;
    return NormalizeYawDegrees(signedYaw + kPlayerModelYawOffsetDeg);
}

float HeadshotDamageForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol):
        return 33.0f;
    case ToWeaponId(GunType::Sniper):
        return 100.0f;
    default:
        return 33.0f;
    }
}

float TorsoshotDamageForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol):
        return 25.0f;
    case ToWeaponId(GunType::Sniper):
        return 75.0f;
    default:
        return 25.0f;
    }
}

float LegshotDamageForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol):
        return 18.0f;
    case ToWeaponId(GunType::Sniper):
        return 50.0f;
    default:
        return 18.0f;
    }
}

float MinSecondsPerShotForWeapon(uint16_t weaponId, float defaultSeconds) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol):
    case ToWeaponId(GunType::Sniper):
    default:
        return defaultSeconds;
    }
}

const char *HitRegionName(HitRegion region) {
    switch (region) {
    case HitRegion::Head:
        return "Head";
    case HitRegion::Body:
        return "Body";
    case HitRegion::Legs:
        return "Legs";
    default:
        return "Unknown";
    }
}

HitRegion RegionFromCacheCode(uint8_t code) {
    switch (code) {
    case 0:
        return HitRegion::Legs;
    case 1:
        return HitRegion::Body;
    case 2:
        return HitRegion::Head;
    default:
        return HitRegion::Unknown;
    }
}

} // namespace CombatRules
