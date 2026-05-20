#pragma once

#include <glm/vec3.hpp>
#include "../voxels/Voxel.hpp"

class ChunkManager;

struct WorldRaycastResult {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 hitPoint{0.0f};
    BlockFace face = BlockFace::PosY;
};

namespace WorldRaycast {
    WorldRaycastResult FindFirstSolidBlockHit(
        const ChunkManager &chunkManager,
        const glm::vec3 &origin,
        const glm::vec3 &dir,
        float maxDistance
    );
} // namespace WorldRaycast
