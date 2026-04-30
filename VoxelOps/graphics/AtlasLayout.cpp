#include "AtlasLayout.hpp"

AtlasLayout::AtlasLayout() {
    tileMap["dirt"] = {0, 0};
    tileMap["grass_side"] = {1, 0};
    tileMap["grass_top"] = {2, 0};
    tileMap["stone"] = {1, 1};
    tileMap["bedrock"] = {2, 1};
    tileMap["sand"] = {3, 0};
    tileMap["log_side"] = {4, 0};
    tileMap["log_top"] = {5, 0};
    tileMap["stone_brick"] = {6, 0};
    tileMap["temple_brick"] = {3, 1};
    tileMap["wood"] = {7, 0};
    tileMap["leaves"] = {0, 1};
    tileMap["iron_ore"] = {1, 3};
    tileMap["iron_block"] = {3, 2};
    tileMap["emerald_ore"] = {4, 2};
    tileMap["red_berry"] = {3, 6};
    tileMap["orange_berry"] = {4, 6};
    tileMap["ruby_gem"] = {0, 3};
    tileMap["sapphire_gem"] = {5, 2};
    tileMap["crafting_table_top"] = {4, 4};
    tileMap["crafting_table_bottom"] = {2, 2};
    tileMap["crafting_table_rl_side"] = {3, 4};
    tileMap["crafting_table_fb_side"] = {5, 4};
    tileMap["bomb_top"] = {7, 7};
    tileMap["bomb_bottom"] = {7, 6};
    tileMap["bomb_side"] = {6, 7};
    tileMap["cactus_top"] = {2, 3};
    tileMap["cactus_bottom"] = {3, 3};
    tileMap["cactus_side"] = {4, 3};
    tileMap["ruby_block"] = {5, 6};
    tileMap["sapphire_block"] = {6, 6};
}

uint8_t AtlasLayout::tileLayerOrDefault(const std::string &tileName) const noexcept {
    const auto it = tileMap.find(tileName);
    if (it == tileMap.end()) {
        return 0;
    }
    const glm::ivec2 tile = it->second;
    return static_cast<uint8_t>(tile.y * TEXTURE_ATLAS_SIZE + tile.x);
}

