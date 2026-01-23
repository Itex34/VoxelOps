#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/fwd.hpp>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

#include "../graphics/TextureAtlas.hpp"

// Face order: +X, -X, +Y, -Y, +Z, -Z
extern const glm::ivec3 faceNormals[6];
extern const glm::ivec3 faceVertices[6][4];
extern const glm::ivec3 faceAxisU[6];
extern const glm::ivec3 faceAxisV[6];
extern const int FACE_CORNER_SIGNS[6][4][2];

extern const std::array<glm::vec2, 4> baseUVs;
extern const uint8_t uvRemap[6][4];


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


std::array<glm::vec2, 4> getTexCoordsForFace(BlockID blockID, int face, const TextureAtlas& atlas);

namespace std {
    template <>
    struct hash<BlockID> {
        size_t operator()(const BlockID& id) const noexcept {
            return static_cast<size_t>(id);
        }
    };
}

struct BlockTexture {
    std::string top;
    std::string bottom;
	std::string RLSide;// for left and right sides
	std::string FBSide;//for back and front sides(needed for crafting table, which doesn't have the same texture on all sides)
};

struct BlockType {
    BlockTexture textures;
    bool isSolid = true;
};

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
    { BlockID::CraftingTable, { {"crafting_table_top", "crafting_table_bottom", "crafting_table_rl_side", "crafting_table_fb_side"}, true}},
    { BlockID::Bomb, { {"bomb_top", "bomb_bottom", "bomb_side", "bomb_side"}, true } },
    { BlockID::Cactus, { {"cactus_top", "cactus_bottom", "cactus_side", "cactus_side"}, true } },
    { BlockID::RubyBlock, { {"ruby_block", "ruby_block", "ruby_block", "ruby_block"}, true } },
	{ BlockID::SapphireBlock, { {"sapphire_block", "sapphire_block", "sapphire_block", "sapphire_block"}, true } },
    { BlockID::Air,   { {"", "", "", ""}, false } }
};

const std::array<std::array<int, 4>, 6> faceUVIndices = {
    // Order indices into texCoords: base = [BL, BR, TR, TL]
    // Face:           BL BR TR TL
    std::array<int, 4>{1, 2, 3, 0}, // +X (Right)
    std::array<int, 4>{1, 2, 3, 0}, // -X (Left)
    std::array<int, 4>{3, 0, 1, 2}, // +Y (Top)
    std::array<int, 4>{0, 1, 2, 3}, // -Y (Bottom)
    std::array<int, 4>{0, 1, 2, 3}, // +Z (Front)
    std::array<int, 4>{0, 1, 2, 3}, // -Z (Back)
};
