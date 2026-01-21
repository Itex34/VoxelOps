#pragma once
#include <cstdint>
#include <string>
#include <array>
#include <unordered_map>
// Server-side version of Voxel.hpp.


enum class BlockID : uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Bedrock,
    Sand,
    Log,
    StoneBrick,
    TempleBrick,
    Wood,
    Leaves,
    IronOre,
    IronBlock,
    EmeraldOre,
    RedBerry,
    OrangeBerry,
    SapphireGem,
    RubyGem,
    CraftingTable,
    Bomb,
    Cactus,
    RubyBlock,
    SapphireBlock,
    COUNT,
};

namespace std {
    template <>
    struct hash<BlockID> {
        size_t operator()(const BlockID& id) const noexcept {
            return static_cast<size_t>(id);
        }
    };
}

/// Keep same structure names so existing server code using them keeps compiling.
struct BlockTexture {
    std::string top;
    std::string bottom;
    std::string RLSide; // left/right sides
    std::string FBSide; // front/back sides (useful for asymmetric blocks)
};

struct BlockType {
    BlockTexture textures;
    bool isSolid = true;
};

/// blockTypes map — kept identical to your original mapping (just moved here).
inline std::unordered_map<BlockID, BlockType> blockTypes = {
    { BlockID::Grass, { {"grass_top", "dirt", "grass_side", "grass_side"}, true } },
    { BlockID::Dirt,  { {"dirt", "dirt", "dirt", "dirt"}, true } },
    { BlockID::Stone, { {"stone", "stone", "stone", "stone"}, true } },
    { BlockID::Bedrock, { {"bedrock", "bedrock", "bedrock", "bedrock"}, true } },
    { BlockID::Sand, { {"sand", "sand", "sand", "sand"}, true } },
    { BlockID::Log, { {"log_top", "log_top", "log_side", "log_side"}, true } },
    { BlockID::StoneBrick, { {"stone_brick", "stone_brick", "stone_brick", "stone_brick"}, true } },
    { BlockID::TempleBrick, { {"temple_brick", "temple_brick", "temple_brick", "temple_brick"}, true } },
    { BlockID::Wood, { {"wood", "wood", "wood", "wood"}, true } },
    { BlockID::Leaves, { {"leaves", "leaves", "leaves", "leaves"}, false } },
    { BlockID::IronOre, { {"iron_ore", "iron_ore", "iron_ore", "iron_ore"}, true } },
    { BlockID::IronBlock, { {"iron_block", "iron_block", "iron_block", "iron_block"}, true } },
    { BlockID::EmeraldOre, { {"emerald_ore", "emerald_ore", "emerald_ore", "emerald_ore"}, true } },
    { BlockID::RedBerry, { {"red_berry", "red_berry", "red_berry", "red_berry"}, true } },
    { BlockID::OrangeBerry, { {"orange_berry", "orange_berry", "orange_berry", "orange_berry"}, true } },
    { BlockID::SapphireGem, { {"sapphire_gem", "sapphire_gem", "sapphire_gem", "sapphire_gem"}, true } },
    { BlockID::RubyGem, { {"ruby_gem", "ruby_gem", "ruby_gem", "ruby_gem"}, true } },
    { BlockID::CraftingTable, { {"crafting_table_top", "crafting_table_bottom", "crafting_table_rl_side", "crafting_table_fb_side"}, true} },
    { BlockID::Bomb, { {"bomb_top", "bomb_bottom", "bomb_side", "bomb_side"}, true } },
    { BlockID::Cactus, { {"cactus_top", "cactus_bottom", "cactus_side", "cactus_side"}, true } },
    { BlockID::RubyBlock, { {"ruby_block", "ruby_block", "ruby_block", "ruby_block"}, true } },
    { BlockID::SapphireBlock, { {"sapphire_block", "sapphire_block", "sapphire_block", "sapphire_block"}, true } },
    { BlockID::Air,   { {"", "", "", ""}, false } }
};

inline bool isSolid(BlockID id) {
    auto it = blockTypes.find(id);
    if (it == blockTypes.end()) return false;
    return it->second.isSolid;
}
