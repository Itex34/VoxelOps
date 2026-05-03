#include "PlayerInventory.hpp"

#include "../../../../Shared/items/Items.hpp"

#include <utility>
#include <vector>

namespace {
    bool tryGetWeaponInventoryItemId(uint16_t weaponId, uint16_t &outItemId) {
        switch (weaponId) {
        case ToWeaponId(GunType::Pistol):
            outItemId = static_cast<uint16_t>(ITEM_PISTOL);
            return true;
        case ToWeaponId(GunType::Sniper):
            outItemId = static_cast<uint16_t>(ITEM_SNIPER);
            return true;
        default:
            return false;
        }
    }

    bool inventoryHasItem(const Inventory &inventory, uint16_t itemId) {
        for (const Slot &slot : inventory.slots()) {
            if (slot.itemId == itemId && slot.quantity > 0) {
                return true;
            }
        }
        return false;
    }

    void fillSnapshotFromInventory(const Inventory &inventory, InventorySnapshot &outSnapshot) {
        outSnapshot.revision = inventory.revision();
        outSnapshot.slots.assign(inventory.slots().begin(), inventory.slots().end());
    }
} // namespace

namespace PlayerInventory {

    bool setEquippedWeapon(
        std::unordered_map<PlayerID, ServerPlayer> &playersById, PlayerID id, uint16_t weaponId
    ) {
        const auto it = playersById.find(id);
        if (it == playersById.end()) {
            return false;
        }

        uint16_t requiredItemId = kInventoryEmptyItemId;
        if (!tryGetWeaponInventoryItemId(weaponId, requiredItemId)) {
            return false;
        }

        ServerPlayer &player = it->second;
        if (!inventoryHasItem(player.inventory, requiredItemId)) {
            return false;
        }

        player.equippedWeaponId = weaponId;
        return true;
    }

    bool applyInventoryAction(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        const InventoryActionRequest &request,
        InventoryActionResult &outResult,
        InventorySnapshot &outSnapshot
    ) {
        outResult = InventoryActionResult{};
        outResult.requestId = request.requestId;
        outResult.accepted = 0;
        outResult.rejectReason = InventoryRejectReason::None;
        outResult.newRevision = 0;
        outResult.changedSlots.clear();
        outSnapshot = InventorySnapshot{};

        const auto it = playersById.find(id);
        if (it == playersById.end()) {
            outResult.rejectReason = InventoryRejectReason::Unsupported;
            return false;
        }

        Inventory &inventory = it->second.inventory;
        if (request.expectedRevision != inventory.revision()) {
            outResult.accepted = 0;
            outResult.rejectReason = InventoryRejectReason::RevisionMismatch;
            outResult.newRevision = inventory.revision();
        } else {
            InventoryRejectReason reject = InventoryRejectReason::None;
            std::vector<uint16_t> changedSlots;
            const bool applied = inventory.applyAction(request.action, reject, changedSlots);
            outResult.accepted = applied ? 1u : 0u;
            outResult.rejectReason = reject;
            outResult.newRevision = inventory.revision();
            outResult.changedSlots = std::move(changedSlots);
        }

        fillSnapshotFromInventory(inventory, outSnapshot);
        return true;
    }

    bool getInventorySnapshot(
        const std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        InventorySnapshot &outSnapshot
    ) {
        const auto it = playersById.find(id);
        if (it == playersById.end()) {
            return false;
        }

        fillSnapshotFromInventory(it->second.inventory, outSnapshot);
        return true;
    }

    bool getInventorySlot(
        const std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        uint16_t slotIndex,
        Slot &outSlot
    ) {
        const auto it = playersById.find(id);
        if (it == playersById.end() || !Inventory::IsValidSlotIndex(slotIndex)) {
            return false;
        }

        outSlot = it->second.inventory.slots()[slotIndex];
        return true;
    }

    bool appendItemsToInventory(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        uint16_t itemId,
        uint16_t quantity,
        uint16_t &outAcceptedQuantity,
        InventorySnapshot *outSnapshot
    ) {
        outAcceptedQuantity = 0;
        if (!Inventory::IsValidItemId(itemId) || quantity == 0) {
            return false;
        }

        const auto it = playersById.find(id);
        if (it == playersById.end()) {
            return false;
        }

        Inventory &inventory = it->second.inventory;
        uint16_t remaining = quantity;
        const bool changed = inventory.appendItems(itemId, quantity, &remaining);
        outAcceptedQuantity = static_cast<uint16_t>(quantity - remaining);

        if (outSnapshot != nullptr && changed) {
            fillSnapshotFromInventory(inventory, *outSnapshot);
        }

        return changed;
    }

} // namespace PlayerInventory
