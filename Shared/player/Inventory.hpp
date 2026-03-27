#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include "../items/Items.hpp"

constexpr uint8_t kHotbarSlots = 6;
constexpr uint8_t kBackpackSlots = 12;
constexpr uint8_t kAmmoSlots = 5;
constexpr uint16_t kInventorySlotCount =
    static_cast<uint16_t>(kHotbarSlots + kBackpackSlots + kAmmoSlots);
constexpr uint16_t kInventoryEmptyItemId = (std::numeric_limits<uint16_t>::max)();


struct Slot {
	uint16_t itemId = kInventoryEmptyItemId;
	uint16_t quantity = 0;
};

enum class InventoryActionType : uint8_t {
	Move = 0,
	Split = 1,
	Swap = 2,
	Drop = 3,
	Use = 4
};

enum class InventoryRejectReason : uint8_t {
	None = 0,
	InvalidSlot = 1,
	InvalidItem = 2,
	SourceEmpty = 3,
	DestinationOccupied = 4,
	StackLimit = 5,
	InvalidAmount = 6,
	RevisionMismatch = 7,
	Unsupported = 8,
	NotUsable = 9
};

struct InventoryAction {
	InventoryActionType type = InventoryActionType::Move;
	uint16_t sourceSlot = 0;
	uint16_t destinationSlot = 0;
	uint16_t amount = 0;
};

class Inventory {
public:
	Inventory();

	[[nodiscard]] const std::array<Slot, kInventorySlotCount>& slots() const noexcept;
	[[nodiscard]] uint32_t revision() const noexcept;

	bool appendItems(uint16_t itemId, uint16_t quantity, uint16_t* outRemaining = nullptr);
	bool applyAction(
		const InventoryAction& action,
		InventoryRejectReason& outReject,
		std::vector<uint16_t>& outChangedSlots
	);

	static bool IsValidSlotIndex(uint16_t slotIndex) noexcept;
	static bool IsEmpty(const Slot& slot) noexcept;
	static bool IsValidItemId(uint16_t itemId) noexcept;
	static bool IsAmmoSlotIndex(uint16_t slotIndex) noexcept;
	static bool IsItemAllowedInSlot(uint16_t itemId, uint16_t slotIndex) noexcept;
	static uint16_t MaxStackForItem(uint16_t itemId) noexcept;

private:
	void NormalizeSlot(uint16_t slotIndex, Slot& slot) noexcept;
	void TouchRevision() noexcept;

	std::array<Slot, kInventorySlotCount> m_slots{};
	uint32_t m_revision = 0;
};
