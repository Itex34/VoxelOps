#pragma once
#include <glm/vec3.hpp>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace BlockPlace {
enum class BlockMode { Block = 0, Wall, Stair, Floor, COUNT };

// the different block modes are just collections of full blocks
struct BlockPatch {
    glm::ivec3 offset{0};
};

struct PlaceResult {

    std::array<uint8_t, 6> chunkNormalsToCenterBlock;
    glm::vec3 position;
};

BlockMode NextMode(BlockMode mode) noexcept;
std::string_view ToString(BlockMode mode) noexcept;

std::vector<glm::ivec3> BuildPlacementOffsets(
    BlockMode mode, const glm::ivec3 &faceNormal, const glm::vec3 &lookDirection
);
std::vector<glm::ivec3> BuildPlacementPositions(
    BlockMode mode,
    const glm::ivec3 &hitBlockWorld,
    const glm::ivec3 &adjacentAirBlockWorld,
    const glm::vec3 &lookDirection,
    bool floorTopFaceForwardAssist
);

} // namespace BlockPlace
