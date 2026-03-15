#pragma once

#include <stdint.h>
#include <string>
#include <array>

#include "../gun/GunType.hpp"


constexpr uint16_t kMaxBlockStack = 999;
constexpr uint16_t kMaxAmmoStack = 255;
constexpr uint16_t kMaxOreStack = 255;
constexpr uint16_t kMaxBerryStack = 12;


constexpr int MAX_ITEMS = 4096;

enum ItemID : uint16_t {
    ITEM_PISTOL,
    ITEM_SNIPER,
    ITEM_ORANGE_BERRY,
    ITEM_RED_BERRY,
    ITEM_PISTOL_AMMO,
    ITEM_COUNT
};


enum class ItemType {
    Gun = 0,    
    Block,  
    Consumable,
    Ammo,
    Ore,
    Other,
    COUNT
};

struct ItemData {
    std::string name;
    ItemType type;
    int maxStack;
    float weight;
};

namespace Items {

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

    extern std::array<ItemData, MAX_ITEMS> ItemDatabase;
}