#pragma once

#include <glm/vec3.hpp>

class ChunkManager;

namespace WorldCollision {

struct QueryResult {
    bool collided = false;
    bool missingChunk = false;
    glm::ivec3 firstMissingChunk{ 0 };
};

QueryResult QueryAabbCollision(
    const ChunkManager& manager,
    const glm::vec3& pos,
    float radius,
    float height,
    bool treatMissingChunkAsSolid
);

} // namespace WorldCollision
