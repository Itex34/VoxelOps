#pragma once

#include <cstdint>
#include <string_view>

enum class GunType : uint16_t {
    Pistol = 0,
    Sniper = 1,
};

constexpr uint16_t ToWeaponId(GunType gunType) noexcept {
    return static_cast<uint16_t>(gunType);
}

constexpr std::string_view GunTypeName(GunType gunType) noexcept {
    switch (gunType) {
    case GunType::Pistol: return "Pistol";
    case GunType::Sniper: return "Sniper";
    default: return "Unknown";
    }
}

constexpr GunType kDefaultGunType = GunType::Pistol;