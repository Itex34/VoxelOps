#pragma once

#include "../../voxels/Voxel.hpp"

#include <cstdint>

// IDs outside the block table have historically been treated as solid.
inline uint32_t giTraceSolidMask() {
    static_assert(static_cast<uint32_t>(BlockID::COUNT) <= 32u,
                  "Extend the GI solidity mask when adding more block IDs.");
    static const uint32_t mask = [] {
        uint32_t result = ~0u;
        for (uint32_t id = 0; id < static_cast<uint32_t>(BlockID::COUNT); ++id) {
            const BlockID block = static_cast<BlockID>(id);
            const auto it = blockTypes.find(block);
            const bool solid = block != BlockID::Air &&
                               (it == blockTypes.end() || it->second.isSolid);
            const uint32_t bit = uint32_t{1} << id;
            result = solid ? (result | bit) : (result & ~bit);
        }
        return result;
    }();
    return mask;
}

inline bool giTraceMaterialIsSolid(uint32_t materialId, uint32_t mask) noexcept {
    return materialId >= 32u || ((mask >> (materialId & 31u)) & 1u) != 0u;
}
