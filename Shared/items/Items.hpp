#pragma once
#include <stdint.h>
#include "../gun/GunType.hpp"




namespace Items {
using GunType = GunType; //so gun types are available through Items

enum class Consumable : uint16_t {
    OrangeBerry = 0,
    RedBerry,


    COUNT
};

enum class AmmoType : uint16_t {
    PistolAmmo = 0,
    ArAmmo,
    SniperAmmo,


    COUNT
};

}

