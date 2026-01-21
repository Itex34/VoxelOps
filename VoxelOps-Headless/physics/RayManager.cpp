#include "RayManager.hpp"

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
            tMax[i] = FLT_MAX;
            tDelta[i] = FLT_MAX;
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


RayResult RayManager::rayHasBlockIntersectSinglePrecise(const Ray& ray, const ChunkManager& chunkManager, float maxDistance) {




}



RayResult RayManager::rayHasBlockIntersectBatch(std::list<Ray>& rays) {
	RayResult result;
	for (auto& ray : rays) {
		return result;
	}
}



