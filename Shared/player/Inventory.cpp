#include "Inventory.hpp"

#include <algorithm>

namespace {
constexpr uint16_t kHotbarStart = 0;
constexpr uint16_t kBackpackStart = kHotbarStart + kHotbarSlots;
constexpr uint16_t kAmmoStart = kBackpackStart + kBackpackSlots;
constexpr uint16_t kAmmoEnd = kAmmoStart + kAmmoSlots;

bool IsAmmoItem(const uint16_t itemId) noexcept {
	if (itemId >= static_cast<uint16_t>(ITEM_COUNT)) {
		return false;
	}
	return Items::ItemDatabase[itemId].type == ItemType::Ammo;
}
}

Inventory::Inventory() {
	for (Slot& slot : m_slots) {
		slot.itemId = kInventoryEmptyItemId;
		slot.quantity = 0;
	}
	m_revision = 0;
}

const std::array<Slot, kInventorySlotCount>& Inventory::slots() const noexcept {
	return m_slots;
}

uint32_t Inventory::revision() const noexcept {
	return m_revision;
}

bool Inventory::IsValidSlotIndex(uint16_t slotIndex) noexcept {
	return slotIndex < kInventorySlotCount;
}

bool Inventory::IsEmpty(const Slot& slot) noexcept {
	return slot.quantity == 0 || slot.itemId == kInventoryEmptyItemId;
}

bool Inventory::IsValidItemId(uint16_t itemId) noexcept {
	return itemId < static_cast<uint16_t>(ITEM_COUNT);
}

bool Inventory::IsAmmoSlotIndex(uint16_t slotIndex) noexcept {
	return slotIndex >= kAmmoStart && slotIndex < kAmmoEnd;
}

bool Inventory::IsItemAllowedInSlot(uint16_t itemId, uint16_t slotIndex) noexcept {
	if (!IsValidSlotIndex(slotIndex) || !IsValidItemId(itemId)) {
		return false;
	}
	if (IsAmmoSlotIndex(slotIndex)) {
		return IsAmmoItem(itemId);
	}
	return true;
}

uint16_t Inventory::MaxStackForItem(uint16_t itemId) noexcept {
	if (!IsValidItemId(itemId)) {
		return 0;
	}
	const int maxStack = Items::ItemDatabase[itemId].maxStack;
	if (maxStack <= 0) {
		return 0;
	}
	return static_cast<uint16_t>(std::min(maxStack, static_cast<int>((std::numeric_limits<uint16_t>::max)())));
}

void Inventory::NormalizeSlot(const uint16_t slotIndex, Slot& slot) noexcept {
	if (!IsValidItemId(slot.itemId) || slot.quantity == 0) {
		slot.itemId = kInventoryEmptyItemId;
		slot.quantity = 0;
		return;
	}
	if (!IsItemAllowedInSlot(slot.itemId, slotIndex)) {
		slot.itemId = kInventoryEmptyItemId;
		slot.quantity = 0;
		return;
	}
	const uint16_t maxStack = MaxStackForItem(slot.itemId);
	if (maxStack == 0) {
		slot.itemId = kInventoryEmptyItemId;
		slot.quantity = 0;
		return;
	}
	if (slot.quantity > maxStack) {
		slot.quantity = maxStack;
	}
}

void Inventory::TouchRevision() noexcept {
	++m_revision;
}

bool Inventory::appendItems(uint16_t itemId, uint16_t quantity, uint16_t* outRemaining) {
	if (outRemaining != nullptr) {
		*outRemaining = quantity;
	}
	if (!IsValidItemId(itemId) || quantity == 0) {
		return false;
	}

	uint16_t remaining = quantity;
	const uint16_t maxStack = MaxStackForItem(itemId);
	if (maxStack == 0) {
		return false;
	}

	bool changed = false;
	const auto stackIntoRange = [&](const uint16_t begin, const uint16_t endExclusive) {
		for (uint16_t i = begin; i < endExclusive && remaining > 0; ++i) {
			Slot& slot = m_slots[i];
			NormalizeSlot(i, slot);
			if (!IsItemAllowedInSlot(itemId, i) || slot.itemId != itemId || slot.quantity >= maxStack) {
				continue;
			}
			const uint16_t freeSpace = static_cast<uint16_t>(maxStack - slot.quantity);
			const uint16_t toAdd = (remaining < freeSpace) ? remaining : freeSpace;
			slot.quantity = static_cast<uint16_t>(slot.quantity + toAdd);
			remaining = static_cast<uint16_t>(remaining - toAdd);
			changed = true;
		}
	};
	const auto fillEmptyRange = [&](const uint16_t begin, const uint16_t endExclusive) {
		for (uint16_t i = begin; i < endExclusive && remaining > 0; ++i) {
			Slot& slot = m_slots[i];
			NormalizeSlot(i, slot);
			if (!IsItemAllowedInSlot(itemId, i) || !IsEmpty(slot)) {
				continue;
			}
			const uint16_t toAdd = (remaining < maxStack) ? remaining : maxStack;
			slot.itemId = itemId;
			slot.quantity = toAdd;
			remaining = static_cast<uint16_t>(remaining - toAdd);
			changed = true;
		}
	};

	const bool ammoItem = IsAmmoItem(itemId);
	if (ammoItem) {
		// Keep ammo primarily in the dedicated ammo bar.
		stackIntoRange(kAmmoStart, kAmmoEnd);
		stackIntoRange(kHotbarStart, kAmmoStart);
		fillEmptyRange(kAmmoStart, kAmmoEnd);
		fillEmptyRange(kHotbarStart, kAmmoStart);
	}
	else {
		stackIntoRange(kHotbarStart, kAmmoStart);
		fillEmptyRange(kHotbarStart, kAmmoStart);
	}

	if (changed) {
		TouchRevision();
	}
	if (outRemaining != nullptr) {
		*outRemaining = remaining;
	}
	return changed;
}

bool Inventory::applyAction(
	const InventoryAction& action,
	InventoryRejectReason& outReject,
	std::vector<uint16_t>& outChangedSlots
) {
	outReject = InventoryRejectReason::None;
	outChangedSlots.clear();

	if (!IsValidSlotIndex(action.sourceSlot)) {
		outReject = InventoryRejectReason::InvalidSlot;
		return false;
	}
	if ((action.type == InventoryActionType::Move ||
		 action.type == InventoryActionType::Split ||
		 action.type == InventoryActionType::Swap) &&
		!IsValidSlotIndex(action.destinationSlot)) {
		outReject = InventoryRejectReason::InvalidSlot;
		return false;
	}

	Slot& source = m_slots[action.sourceSlot];
	NormalizeSlot(action.sourceSlot, source);
	if (IsEmpty(source) && action.type != InventoryActionType::Swap) {
		outReject = InventoryRejectReason::SourceEmpty;
		return false;
	}

	switch (action.type) {
	case InventoryActionType::Move: {
		if (action.sourceSlot == action.destinationSlot) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}
		Slot& destination = m_slots[action.destinationSlot];
		NormalizeSlot(action.destinationSlot, destination);

		const uint16_t requestedAmount = (action.amount == 0) ? source.quantity : action.amount;
		if (requestedAmount == 0 || requestedAmount > source.quantity) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}
		if (!IsItemAllowedInSlot(source.itemId, action.destinationSlot)) {
			outReject = InventoryRejectReason::InvalidItem;
			return false;
		}

		if (IsEmpty(destination)) {
			destination.itemId = source.itemId;
			destination.quantity = requestedAmount;
			source.quantity = static_cast<uint16_t>(source.quantity - requestedAmount);
			NormalizeSlot(action.sourceSlot, source);
			NormalizeSlot(action.destinationSlot, destination);
			outChangedSlots.push_back(action.sourceSlot);
			outChangedSlots.push_back(action.destinationSlot);
			TouchRevision();
			return true;
		}

		if (destination.itemId != source.itemId) {
			outReject = InventoryRejectReason::DestinationOccupied;
			return false;
		}

		const uint16_t maxStack = MaxStackForItem(source.itemId);
		if (maxStack == 0 || destination.quantity >= maxStack) {
			outReject = InventoryRejectReason::StackLimit;
			return false;
		}

		const uint16_t freeSpace = static_cast<uint16_t>(maxStack - destination.quantity);
		const uint16_t moved = std::min<uint16_t>(freeSpace, requestedAmount);
		if (moved == 0) {
			outReject = InventoryRejectReason::StackLimit;
			return false;
		}

		destination.quantity = static_cast<uint16_t>(destination.quantity + moved);
		source.quantity = static_cast<uint16_t>(source.quantity - moved);
		NormalizeSlot(action.sourceSlot, source);
		NormalizeSlot(action.destinationSlot, destination);
		outChangedSlots.push_back(action.sourceSlot);
		outChangedSlots.push_back(action.destinationSlot);
		TouchRevision();
		return true;
	}
	case InventoryActionType::Split: {
		if (action.sourceSlot == action.destinationSlot) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}
		Slot& destination = m_slots[action.destinationSlot];
		NormalizeSlot(action.destinationSlot, destination);
		if (!IsEmpty(destination)) {
			outReject = InventoryRejectReason::DestinationOccupied;
			return false;
		}
		if (source.quantity < 2) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}

		const uint16_t maxStack = MaxStackForItem(source.itemId);
		if (maxStack == 0) {
			outReject = InventoryRejectReason::InvalidItem;
			return false;
		}

		const uint16_t defaultAmount = static_cast<uint16_t>(source.quantity / 2);
		const uint16_t requestedAmount = (action.amount == 0) ? defaultAmount : action.amount;
		if (requestedAmount == 0 || requestedAmount >= source.quantity || requestedAmount > maxStack) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}
		if (!IsItemAllowedInSlot(source.itemId, action.destinationSlot)) {
			outReject = InventoryRejectReason::InvalidItem;
			return false;
		}

		destination.itemId = source.itemId;
		destination.quantity = requestedAmount;
		source.quantity = static_cast<uint16_t>(source.quantity - requestedAmount);
		NormalizeSlot(action.sourceSlot, source);
		NormalizeSlot(action.destinationSlot, destination);
		outChangedSlots.push_back(action.sourceSlot);
		outChangedSlots.push_back(action.destinationSlot);
		TouchRevision();
		return true;
	}
	case InventoryActionType::Swap: {
		if (action.sourceSlot == action.destinationSlot) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}
		Slot& destination = m_slots[action.destinationSlot];
		NormalizeSlot(action.destinationSlot, destination);
		if (!IsEmpty(source) && !IsItemAllowedInSlot(source.itemId, action.destinationSlot)) {
			outReject = InventoryRejectReason::InvalidItem;
			return false;
		}
		if (!IsEmpty(destination) && !IsItemAllowedInSlot(destination.itemId, action.sourceSlot)) {
			outReject = InventoryRejectReason::InvalidItem;
			return false;
		}
		std::swap(source, destination);
		NormalizeSlot(action.sourceSlot, source);
		NormalizeSlot(action.destinationSlot, destination);
		outChangedSlots.push_back(action.sourceSlot);
		outChangedSlots.push_back(action.destinationSlot);
		TouchRevision();
		return true;
	}
	case InventoryActionType::Drop: {
		const uint16_t requestedAmount = (action.amount == 0) ? source.quantity : action.amount;
		if (requestedAmount == 0 || requestedAmount > source.quantity) {
			outReject = InventoryRejectReason::InvalidAmount;
			return false;
		}
		source.quantity = static_cast<uint16_t>(source.quantity - requestedAmount);
		NormalizeSlot(action.sourceSlot, source);
		outChangedSlots.push_back(action.sourceSlot);
		TouchRevision();
		return true;
	}
	case InventoryActionType::Use: {
		if (!IsValidItemId(source.itemId)) {
			outReject = InventoryRejectReason::InvalidItem;
			return false;
		}
		const ItemType type = Items::ItemDatabase[source.itemId].type;
		if (type != ItemType::Consumable) {
			outReject = InventoryRejectReason::NotUsable;
			return false;
		}
		source.quantity = static_cast<uint16_t>(source.quantity - 1);
		NormalizeSlot(action.sourceSlot, source);
		outChangedSlots.push_back(action.sourceSlot);
		TouchRevision();
		return true;
	}
	default:
		outReject = InventoryRejectReason::Unsupported;
		return false;
	}
}
