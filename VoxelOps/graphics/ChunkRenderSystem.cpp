#include "ChunkRenderSystem.hpp"

#include "ChunkManager.hpp"
#include "../player/Player.hpp"

void ChunkRenderSystem::renderChunks(
    ChunkManager& cm,
    Shader& shader,
    Frustum& frustum,
    Player& player,
    int maxRenderDistance
) {
    const glm::ivec3 playerChunkPos =
        cm.worldToChunkPos(glm::ivec3(player.getPosition()));

    if (!cm.m_tileInfoInitialized) {
        for (size_t i = 0; i < 256; ++i) {
            cm.m_tileInfo[i] = glm::vec4(0, 0, 1, 1);
        }

        for (const auto& [name, tilePos] : cm.atlas.tileMap) {
            int tileX = tilePos.x;
            int tileY = tilePos.y;
            int index = tileY * TEXTURE_ATLAS_SIZE + tileX;

            auto [min, max] = cm.atlas.getUVRect(name);
            cm.m_tileInfo[index] = glm::vec4(min, max - min);
        }

        shader.setVec4v("u_tileInfo", 256, cm.m_tileInfo);
        shader.setFloat("u_chunkSize", float(CHUNK_SIZE));

        cm.m_tileInfoInitialized = true;
    }

    const int maxDistSq = maxRenderDistance * maxRenderDistance;

    for (auto& [regionPos, region] : cm.regions) {
        glm::vec3 regionMin = glm::vec3(regionPos * REGION_SIZE * CHUNK_SIZE);
        glm::vec3 regionMax = regionMin + glm::vec3(REGION_SIZE * CHUNK_SIZE);
        if (!frustum.isBoxVisible(regionMin, regionMax)) {
            continue;
        }

        RegionMeshBuffer& gpu = *region.gpu;
        for (const auto& [chunkPos, mesh] : region.chunks) {
            if (!mesh.valid) {
                continue;
            }

            glm::ivec3 d = chunkPos - playerChunkPos;
            if (d.x * d.x + d.y * d.y + d.z * d.z > maxDistSq) {
                continue;
            }

            glm::vec3 min = glm::vec3(chunkPos * CHUNK_SIZE);
            glm::vec3 max = min + glm::vec3(CHUNK_SIZE);
            if (!frustum.isBoxVisible(min, max)) {
                continue;
            }

            glm::mat4 model(1.0f);
            model[3] = glm::vec4(min, 1.0f);
            shader.setMat4("model", model);
            gpu.drawChunkMesh(mesh);
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
