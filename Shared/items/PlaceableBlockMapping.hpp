#pragma once

#include "Items.hpp"

#include <cstdint>
#include <optional>

namespace PlaceableBlockMapping {

    struct Entry {
        uint16_t itemId;
        uint8_t blockId;
    };

    // Keep IDs aligned with BlockID in client/server voxel enums.
    constexpr Entry kEntries[] = {
        {static_cast<uint16_t>(ITEM_DIRT_BLOCK), 2u},      // BlockID::Dirt
        {static_cast<uint16_t>(ITEM_RUBY_BLOCK), 21u},     // BlockID::RubyBlock
        {static_cast<uint16_t>(ITEM_SAPPHIRE_BLOCK), 22u}, // BlockID::SapphireBlock
        {static_cast<uint16_t>(ITEM_IRON_BLOCK), 12u}, // BlockID::IronBlock

    };

    inline std::optional<uint8_t> blockIdFromItemId(uint16_t itemId) {
        for (const Entry &entry : kEntries) {
            if (entry.itemId == itemId) {
                return entry.blockId;
            }
        }
        return std::nullopt;
    }

    inline std::optional<uint16_t> itemIdFromBlockId(uint8_t blockId) {
        for (const Entry &entry : kEntries) {
            if (entry.blockId == blockId) {
                return entry.itemId;
            }
        }
        return std::nullopt;
    }

    inline bool isPlaceableBlockItem(uint16_t itemId) {
        return blockIdFromItemId(itemId).has_value();
    }

} // namespace PlaceableBlockMapping

