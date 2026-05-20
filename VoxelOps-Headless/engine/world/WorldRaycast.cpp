#include "WorldRaycast.hpp"

#include "ChunkManager.hpp"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace WorldRaycast {
    namespace {
        BlockFace faceFromEntryAxis(int axis, int stepSign, const glm::vec3 &rayDir) {
            if (axis < 0) {
                const glm::vec3 absDir = glm::abs(rayDir);
                if (absDir.x >= absDir.y && absDir.x >= absDir.z) {
                    return (rayDir.x > 0.0f) ? BlockFace::NegX : BlockFace::PosX;
                }
                if (absDir.y >= absDir.z) {
                    return (rayDir.y > 0.0f) ? BlockFace::NegY : BlockFace::PosY;
                }
                return (rayDir.z > 0.0f) ? BlockFace::NegZ : BlockFace::PosZ;
            }
            switch (axis) {
            case 0:
                return (stepSign > 0) ? BlockFace::NegX : BlockFace::PosX;
            case 1:
                return (stepSign > 0) ? BlockFace::NegY : BlockFace::PosY;
            case 2:
            default:
                return (stepSign > 0) ? BlockFace::NegZ : BlockFace::PosZ;
            }
        }
    } // namespace

    WorldRaycastResult FindFirstSolidBlockHit(
        const ChunkManager &chunkManager,
        const glm::vec3 &origin,
        const glm::vec3 &dir,
        float maxDistance
    ) {
        if (!std::isfinite(maxDistance) || maxDistance <= 0.0f) {
            return {};
        }
        const float dirLenSq = glm::dot(dir, dir);
        if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
            return {};
        }

        const glm::vec3 rayDir = glm::normalize(dir);
        glm::ivec3 currentBlock = glm::ivec3(glm::floor(origin));
        const glm::ivec3 step = glm::sign(rayDir);

        glm::vec3 tMax(0.0f);
        glm::vec3 tDelta(0.0f);
        for (int i = 0; i < 3; ++i) {
            if (rayDir[i] != 0.0f) {
                const float nextBoundary =
                    (step[i] > 0) ? (currentBlock[i] + 1.0f) : currentBlock[i];
                tMax[i] = (nextBoundary - origin[i]) / rayDir[i];
                tDelta[i] = std::abs(1.0f / rayDir[i]);
            } else {
                tMax[i] = std::numeric_limits<float>::max();
                tDelta[i] = std::numeric_limits<float>::max();
            }
        }

        constexpr int kMaxDdaSteps = 2048;
        float traveled = 0.0f;
        int enteredAxis = -1;
        int enteredStepSign = 1;
        const ServerChunk *cachedChunk = nullptr;
        glm::ivec3 cachedChunkCoords(std::numeric_limits<int>::max());
        for (int i = 0; i < kMaxDdaSteps; ++i) {
            const glm::ivec3 chunkCoords = chunkManager.worldToChunkPos(currentBlock);
            if (chunkCoords != cachedChunkCoords) {
                cachedChunkCoords = chunkCoords;
                cachedChunk = chunkManager.getChunkIfExists(chunkCoords);
            }
            if (cachedChunk) {
                const glm::ivec3 blockInChunk = currentBlock - cachedChunk->getWorldPosition();
                if (cachedChunk->getBlockUnchecked(
                        blockInChunk.x, blockInChunk.y, blockInChunk.z
                    ) != BlockID::Air) {
                    const BlockFace hitFace =
                        faceFromEntryAxis(enteredAxis, enteredStepSign, rayDir);
                    return {
                        .hit = true,
                        .distance = traveled,
                        .hitPoint = origin + rayDir * traveled,
                        .face = hitFace
                    };
                }
            }

            const float nextT = std::min({tMax.x, tMax.y, tMax.z});
            if (nextT > maxDistance) {
                break;
            }

            if (tMax.x < tMax.y) {
                if (tMax.x < tMax.z) {
                    traveled = tMax.x;
                    enteredAxis = 0;
                    enteredStepSign = step.x;
                    currentBlock.x += step.x;
                    tMax.x += tDelta.x;
                } else {
                    traveled = tMax.z;
                    enteredAxis = 2;
                    enteredStepSign = step.z;
                    currentBlock.z += step.z;
                    tMax.z += tDelta.z;
                }
            } else {
                if (tMax.y < tMax.z) {
                    traveled = tMax.y;
                    enteredAxis = 1;
                    enteredStepSign = step.y;
                    currentBlock.y += step.y;
                    tMax.y += tDelta.y;
                } else {
                    traveled = tMax.z;
                    enteredAxis = 2;
                    enteredStepSign = step.z;
                    currentBlock.z += step.z;
                    tMax.z += tDelta.z;
                }
            }
        }

        return {};
    }

} // namespace WorldRaycast
