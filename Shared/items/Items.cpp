#include "Items.hpp"

namespace Items {
	std::array<ItemData, MAX_ITEMS> ItemDatabase = {
		ItemData{ "Pistol", ItemType::Gun, 1, 2.0f },
		ItemData{ "Sniper Rifle", ItemType::Gun, 1, 4.0f },
		ItemData{ "Orange Berry", ItemType::Consumable, kMaxBerryStack, 0.1f },
		ItemData{ "Red Berry", ItemType::Consumable, kMaxBerryStack, 0.1f },
		ItemData{ "Pistol Ammo", ItemType::Ammo, kMaxAmmoStack, 0.05f },
	};

}