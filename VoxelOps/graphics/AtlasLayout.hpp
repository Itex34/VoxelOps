#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

constexpr int TEXTURE_ATLAS_SIZE = 16; // 16x16 tiles in the atlas
constexpr int TILE_RESOLUTION = 16;    // each tile is 16x16 pixels

struct AtlasLayout {
    AtlasLayout();

    std::unordered_map<std::string, glm::ivec2> tileMap;

    [[nodiscard]] uint8_t tileLayerOrDefault(const std::string &tileName) const noexcept;
};
