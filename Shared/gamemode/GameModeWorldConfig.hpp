#pragma once

#include "GameMode.hpp"
#include "../world/Constants.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

struct ChunkWorldBounds {
    glm::ivec3 minChunk;
    glm::ivec3 maxChunk;

    [[nodiscard]] int sizeX() const noexcept {
        return maxChunk.x - minChunk.x + 1;
    }

    [[nodiscard]] int sizeY() const noexcept {
        return maxChunk.y - minChunk.y + 1;
    }

    [[nodiscard]] int sizeZ() const noexcept {
        return maxChunk.z - minChunk.z + 1;
    }

    [[nodiscard]] std::size_t slotCount() const noexcept {
        return static_cast<std::size_t>(sizeX()) * static_cast<std::size_t>(sizeY()) *
               static_cast<std::size_t>(sizeZ());
    }

    [[nodiscard]] bool contains(const glm::ivec3 &pos) const noexcept {
        return pos.x >= minChunk.x && pos.x <= maxChunk.x && pos.y >= minChunk.y &&
               pos.y <= maxChunk.y && pos.z >= minChunk.z && pos.z <= maxChunk.z;
    }
};

inline int ChunkFloorDiv(int a, int b) noexcept {
    int q = a / b;
    const int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

inline ChunkWorldBounds GetChunkWorldBounds(GameMode mode, int chunkSize) noexcept {
    const int minChunkY = ChunkFloorDiv(WORLD_MIN_Y, chunkSize);
    const int maxChunkY = ChunkFloorDiv(WORLD_MAX_Y, chunkSize);

    switch (mode) {
    case GameMode::FFA:
        return {{WORLD_MIN_X, minChunkY, WORLD_MIN_Z}, {WORLD_MAX_X, maxChunkY, WORLD_MAX_Z}};
    case GameMode::BattleRoyale:
        return {{-41, minChunkY, -41}, {40, maxChunkY, 40}};
    case GameMode::Survival:
        return {{-82, minChunkY, -82}, {81, maxChunkY, 81}};
    }

    return {{WORLD_MIN_X, minChunkY, WORLD_MIN_Z}, {WORLD_MAX_X, maxChunkY, WORLD_MAX_Z}};
}
