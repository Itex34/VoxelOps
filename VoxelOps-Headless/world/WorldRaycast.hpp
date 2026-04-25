#pragma once

#include <glm/vec3.hpp>

class ChunkManager;

namespace WorldRaycast {
bool FindFirstSolidBlockHit(const ChunkManager &chunkManager,
                            const glm::vec3 &origin,
                            const glm::vec3 &dir,
                            float maxDistance,
                            float &outDistance,
                            glm::vec3 &outHitPoint);
} // namespace WorldRaycast
