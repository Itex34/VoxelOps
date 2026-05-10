#pragma once

#include "../ServerPlayer.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <utility>

namespace PlayerInventory {
    bool setEquippedWeapon(
        std::unordered_map<PlayerID, ServerPlayer> &playersById, PlayerID id, uint16_t weaponId
    );

    bool applyInventoryAction(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        const InventoryActionRequest &request,
        InventoryActionResult &outResult,
        InventorySnapshot &outSnapshot
    );

    bool getInventorySnapshot(
        const std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        InventorySnapshot &outSnapshot
    );

    bool getInventorySlot(
        const std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        uint16_t slotIndex,
        Slot &outSlot
    );

    bool appendItemsToInventory(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        uint16_t itemId,
        uint16_t quantity,
        uint16_t &outAcceptedQuantity,
        InventorySnapshot *outSnapshot = nullptr
    );

    bool consumeItemsFromInventory(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        const std::vector<std::pair<uint16_t, uint16_t>> &itemQuantities,
        InventorySnapshot *outSnapshot = nullptr
    );
} // namespace PlayerInventory
