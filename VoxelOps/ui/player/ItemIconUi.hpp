#pragma once

#include "../../../Shared/items/PlaceableBlockMapping.hpp"
#include "../../../Shared/player/Inventory.hpp"
#include "../../graphics/TextureHandle.hpp"
#include "../../voxels/Voxel.hpp"
#include "../native/NativeUiSystem.hpp"

#include <optional>

namespace ItemIconUi {

inline TextureHandle blockTextureForSlot(NativeUiSystem &nativeUi, const Slot &slot) {
    if (Inventory::IsEmpty(slot) || !Inventory::IsValidItemId(slot.itemId)) {
        return 0;
    }

    const std::optional<std::uint8_t> blockId = PlaceableBlockMapping::blockIdFromItemId(slot.itemId);
    if (!blockId.has_value()) {
        return 0;
    }

    const BlockID typedBlockId = static_cast<BlockID>(*blockId);
    const auto blockIt = blockTypes.find(typedBlockId);
    if (blockIt == blockTypes.end()) { 
        return 0;
    }

    const BlockTexture &textures = blockIt->second.textures;
    const std::string &tileName = !textures.top.empty() ? textures.top : textures.RLSide;
    return nativeUi.atlasTile(tileName);
}

} // namespace ItemIconUi
