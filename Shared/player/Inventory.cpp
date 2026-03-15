#include "Inventory.hpp"


Inventory::Inventory() {
	hotbar[0] = { 1, 10 };
	hotbar[1] = { 2, 5 };
	hotbar[2] = { 3, 20 };
	hotbar[3] = { 4, 15 };
	hotbar[4] = { 5, 8 };
	hotbar[5] = { 6, 12 };
}


void Inventory::appendItems(uint16_t itemId, uint16_t quantity) {













	//for (auto& slot : hotbar) {
	//	if (slot.itemId == itemId && slot.quantity < kMaxBlockStack) {
	//		uint16_t space = kMaxBlockStack - slot.quantity;
	//		uint16_t toAdd = std::min(space, quantity);
	//		slot.quantity += toAdd;
	//		quantity -= toAdd;
	//		if (quantity == 0) return;
	//	}
	//}
	//for (auto& slot : backpack) {
	//	if (slot.itemId == itemId && slot.quantity < kMaxBlockStack) {
	//		uint16_t space = kMaxBlockStack - slot.quantity;
	//		uint16_t toAdd = std::min(space, quantity);
	//		slot.quantity += toAdd;
	//		quantity -= toAdd;
	//		if (quantity == 0) return;
	//	}
	//}
	//for (auto& slot : ammo) {
	//	if (slot.itemId == itemId && slot.quantity < kMaxAmmoStack) {
	//		uint16_t space = kMaxAmmoStack - slot.quantity;
	//		uint16_t toAdd = std::min(space, quantity);
	//		slot.quantity += toAdd;
	//		quantity -= toAdd;
	//		if (quantity == 0) return;
	//	}
	//}
}