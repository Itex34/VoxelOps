#include "ChunkRenderSystem.hpp"

#include "ChunkManager.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>

void ChunkRenderSystem::renderChunks(
    ChunkManager& cm,
    Shader& shader,
    Frustum& frustum,
    const glm::vec3& viewPosition,
    int maxRenderDistance
) {
    const glm::vec3 playerPos = viewPosition;
    const glm::ivec3 playerBlockPos(
        static_cast<int>(std::floor(playerPos.x)),
        static_cast<int>(std::floor(playerPos.y)),
        static_cast<int>(std::floor(playerPos.z))
    );
    const glm::ivec3 playerChunkPos = cm.worldToChunkPos(playerBlockPos);

    size_t regionCount = 0;
    size_t validMeshCount = 0;
    size_t drawnCount = 0;
    size_t distCullCount = 0;
    size_t frustumCullCount = 0;

    for (auto& [regionPos, region] : cm.regions) {
        ++regionCount;
        glm::vec3 regionMin = glm::vec3(regionPos * REGION_SIZE * CHUNK_SIZE);
        glm::vec3 regionMax = regionMin + glm::vec3(REGION_SIZE * CHUNK_SIZE);
        if (!frustum.isBoxVisible(regionMin, regionMax)) {
            frustumCullCount += region.chunks.size();
            continue;
        }

        RegionMeshBuffer& gpu = *region.gpu;
        for (const auto& [chunkPos, mesh] : region.chunks) {
            if (!mesh.valid) {
                continue;
            }
            ++validMeshCount;

            glm::ivec3 d = chunkPos - playerChunkPos;
            // Client-side render culling uses radial distance in XZ.
            const int64_t dist2 =
                static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
                static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
            const int64_t radius2 =
                static_cast<int64_t>(maxRenderDistance) * static_cast<int64_t>(maxRenderDistance);
            if (dist2 > radius2) {
                ++distCullCount;
                continue;
            }

            glm::vec3 min = glm::vec3(chunkPos * CHUNK_SIZE);
            glm::vec3 max = min + glm::vec3(CHUNK_SIZE);
            if (!frustum.isBoxVisible(min, max)) {
                ++frustumCullCount;
                continue;
            }

            glm::mat4 model(1.0f);
            model[3] = glm::vec4(min, 1.0f);
            shader.setMat4("model", model);
            gpu.drawChunkMesh(mesh);
            ++drawnCount;
        }
    }
}

void ChunkRenderSystem::renderChunkBorders(
    ChunkManager& cm,
    glm::mat4& view,
    glm::mat4& projection
) {
    cm.debugShader->use();
    cm.debugShader->setMat4("projection", projection);
    cm.debugShader->setMat4("view", view);
    cm.debugShader->setVec3("color", glm::vec3(0.0f, 1.0f, 0.0f));

    glBindVertexArray(cm.wireVAO);

    for (int z = WORLD_MIN_Z; z <= WORLD_MAX_Z; ++z) {
        for (int x = WORLD_MIN_X; x <= WORLD_MAX_X; ++x) {
            glm::ivec3 pos(x, 0, z);
            if (!cm.inBounds(pos)) {
                continue;
            }

            glm::vec3 worldPos = glm::vec3(pos.x * CHUNK_SIZE, 0.0f, pos.z * CHUNK_SIZE);
            glm::vec3 scale = glm::vec3(CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
            model = glm::scale(model, scale);
            cm.debugShader->setMat4("model", model);
            glDrawArrays(GL_LINES, 0, 24);
        }
    }
}
