#include "RayManager.hpp"
#include "../player/Player.hpp"

RayManager::RayManager() {

}

RayResult RayManager::rayHasBlockIntersectSingle(const Ray& ray, const ChunkManager& chunkManager, float maxDistance) {
    RayResult result{};
    result.hit = false;
    result.distance = maxDistance;
    
    glm::vec3 rayDir = glm::normalize(ray.direction);
    //// ====== 1. Setup DDA for block traversal ======
    glm::ivec3 currentBlock = glm::floor(ray.origin); // starting voxel

    glm::ivec3 step = glm::sign(rayDir); // step: -1, 0, or +1

    glm::vec3 tMax;
    glm::vec3 tDelta;

    // Compute tMax and tDelta
    for (int i = 0; i < 3; i++) {
        if (rayDir[i] != 0.0f) {
            float nextBoundary = (step[i] > 0)
                ? (currentBlock[i] + 1.0f)
                : (currentBlock[i]);
            tMax[i] = (nextBoundary - ray.origin[i]) / rayDir[i];
            tDelta[i] = std::abs(1.0f / rayDir[i]);
        }
        else {
            tMax[i] = std::numeric_limits<float>::max();
            tDelta[i] = std::numeric_limits<float>::max();
        }
    }

    // ====== Traverse blocks ======
    for (int i = 0; i < 1024; i++) { // safety cap
        float traveled = std::min({ tMax.x, tMax.y, tMax.z });
        if (traveled > maxDistance)
            break;

        // Get the chunk this block belongs to
        glm::ivec3 chunkCoords = chunkManager.worldToChunkPos(currentBlock);

        if (chunkManager.getChunks().contains(chunkCoords)) {
            const Chunk& chunk = chunkManager.getChunks().at(chunkCoords);

            glm::ivec3 blockInChunk = currentBlock - chunk.getWorldPosition();

            if (chunk.inBounds(blockInChunk.x, blockInChunk.y, blockInChunk.z)) {


                BlockID block = chunk.getBlockUnchecked(blockInChunk.x, blockInChunk.y, blockInChunk.z);
                if (block != BlockID::Air) {
                    result.hit = true;
                    result.hitBlockWorld = currentBlock;
                    result.hitChunk = chunkCoords;
                    result.distance = traveled;
                    return result;
                }
            }
        }

        // ====== Step to next block ======
        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                currentBlock.x += step.x;
                tMax.x += tDelta.x;
            }
            else {
                currentBlock.z += step.z;
                tMax.z += tDelta.z;
            }
        }
        else {
            if (tMax.y < tMax.z) {
                currentBlock.y += step.y;
                tMax.y += tDelta.y;
            }
            else {
                currentBlock.z += step.z;
                tMax.z += tDelta.z;
            }
        }


    }
    

    return result; // no hit found
}



RayShootHit RayManager::rayShoot(
    const glm::vec3& origin,
    const glm::vec3& dir,
    const ChunkManager& chunkManager,
    const std::vector<Player*>& players,
    float maxDistance)
{
    RayShootHit result{};
    result.hit = false;
    result.type = RayShootHit::Type::None;
    result.distance = maxDistance;

    glm::vec3 rayDir = glm::normalize(dir);
    glm::ivec3 currentBlock = glm::ivec3(glm::floor(origin));
    glm::ivec3 step = glm::sign(rayDir);

    glm::vec3 tMax;
    glm::vec3 tDelta;
    for (int i = 0; i < 3; ++i) {
        if (rayDir[i] != 0.0f) {
            float nextBoundary = (step[i] > 0) ? (currentBlock[i] + 1.0f) : static_cast<float>(currentBlock[i]);
            tMax[i] = (nextBoundary - origin[i]) / rayDir[i];
            tDelta[i] = std::abs(1.0f / rayDir[i]);
        }
        else {
            tMax[i] = tDelta[i] = std::numeric_limits<float>::max();
        }
    }

    // check starting block immediately
    {
        glm::ivec3 chunkCoords = chunkManager.worldToChunkPos(currentBlock);
        if (chunkManager.getChunks().contains(chunkCoords)) {
            const Chunk& chunk = chunkManager.getChunks().at(chunkCoords);
            glm::ivec3 blockInChunk = currentBlock - chunk.getWorldPosition();
            if (chunk.inBounds(blockInChunk.x, blockInChunk.y, blockInChunk.z)) {
                BlockID b = chunk.getBlockUnchecked(blockInChunk.x, blockInChunk.y, blockInChunk.z);
                if (b != BlockID::Air) {
                    result.hit = true;
                    result.type = RayShootHit::Type::Block;
                    result.blockPos = currentBlock;
                    result.chunkPos = chunkCoords;
                    result.hitPoint = origin;
                    result.distance = 0.0f;
                    // We still should check players: if a player is at distance 0 as well, player can override
                }
            }
        }
    }

    // DDA traversal: if we already hit a block at distance 0 above, we still want to
    // allow player hits at < result.distance. If not hit yet, traverse until first block hit.
    const int MAX_STEPS = 1024;
    for (int i = 0; i < MAX_STEPS; ++i) {
        float traveled = std::min({ tMax.x, tMax.y, tMax.z });
        if (traveled > maxDistance) break;

        // If current best hit is closer than the next boundary, we can stop block traversal.
        if (result.hit && result.distance <= traveled) break;

        // step to next voxel
        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                currentBlock.x += step.x; tMax.x += tDelta.x;
            }
            else {
                currentBlock.z += step.z; tMax.z += tDelta.z;
            }
        }
        else {
            if (tMax.y < tMax.z) {
                currentBlock.y += step.y; tMax.y += tDelta.y;
            }
            else {
                currentBlock.z += step.z; tMax.z += tDelta.z;
            }
        }

        glm::ivec3 chunkCoords = chunkManager.worldToChunkPos(currentBlock);
        if (chunkManager.getChunks().contains(chunkCoords)) {
            const Chunk& chunk = chunkManager.getChunks().at(chunkCoords);
            glm::ivec3 blockInChunk = currentBlock - chunk.getWorldPosition();
            if (chunk.inBounds(blockInChunk.x, blockInChunk.y, blockInChunk.z)) {
                BlockID b = chunk.getBlockUnchecked(blockInChunk.x, blockInChunk.y, blockInChunk.z);
                if (b != BlockID::Air) {
                    float hitDistance = std::min({ tMax.x, tMax.y, tMax.z }); // distance at which we entered this block
                    if (!result.hit || hitDistance < result.distance) {
                        result.hit = true;
                        result.type = RayShootHit::Type::Block;
                        result.blockPos = currentBlock;
                        result.chunkPos = chunkCoords;
                        result.hitPoint = origin + rayDir * hitDistance;
                        result.distance = hitDistance;
                    }
                    // we don't break immediately here because a player could be closer than this block.
                    // but since we keep result.distance as the closest block, we'll use it to prune player hits later.
                }
            }
        }
    }

    // ===== Check players (only accept hits closer than the nearest block found so far) =====
    for (Player* player : players) {
        if (!player) continue;

        glm::mat4 modelMatrix = player->getModelMatrix();
        const auto& hitboxes = player->getHitboxes();

        HitResult hit = HitboxManager::raycastHitboxes(origin, dir, hitboxes, modelMatrix, maxDistance);

        if (hit.hit && hit.distance < result.distance) {
            result.hit = true;
            result.type = RayShootHit::Type::Player;
            result.player = player;
            result.region = hit.region;
            result.hitPoint = hit.hitPointWorld;
            result.distance = hit.distance;
        }
    }

    return result;
}



RayResult RayManager::rayHasBlockIntersectBatch(std::list<Ray>& rays) {
	RayResult result;
	for (auto& ray : rays) {
		return result;
	}
}



