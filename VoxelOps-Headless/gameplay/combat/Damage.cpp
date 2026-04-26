#include "Damage.hpp"

#include "../../network/gameplay/CombatRules.hpp"

namespace Damage {

float ComputeDamage(uint16_t weaponId, HitRegion region) {
    switch (region) {
    case HitRegion::Head:
        return CombatRules::HeadshotDamageForWeapon(weaponId);
    case HitRegion::Body:
        return CombatRules::TorsoshotDamageForWeapon(weaponId);
    case HitRegion::Legs:
        return CombatRules::LegshotDamageForWeapon(weaponId);
    case HitRegion::Unknown:
    default:
        return CombatRules::TorsoshotDamageForWeapon(weaponId);
    }
}

DamageResolution ResolveDamage(PlayerManager &playerManager,
                               PlayerID targetId,
                               uint16_t weaponId,
                               HitRegion region) {
    DamageResolution out{};
    out.damage = ComputeDamage(weaponId, region);
    out.applied = playerManager.applyDamage(targetId, out.damage, out.healthAfter, out.killed);
    return out;
}

} // namespace Damage

