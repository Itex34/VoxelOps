#pragma once
#include <stdint.h>
#include <vector>
#include "../items/Items.hpp"

constexpr uint8_t kHotbarSlots = 6;
constexpr uint8_t kBackpackSlots = 12;
constexpr uint8_t kAmmoSlots = 6;


struct Slot {
	uint16_t itemId = 0;
	uint16_t quantity = 0;
};

class Inventory {
public:
	Inventory();

	void appendItems(uint16_t itemId, uint16_t quantity);

	std::array<Slot, kHotbarSlots> hotbar;
	std::array<Slot, kBackpackSlots> backpack;
	std::array<Slot, kAmmoSlots> ammo;

private:
	bool m_hotbarFull = false;
	bool m_backpackFull = false;
	bool m_AmmoFull = false;
};