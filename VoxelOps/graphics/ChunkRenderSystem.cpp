#include "ChunkRenderSystem.hpp"

#include "ChunkManager.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

void ChunkRenderSystem::renderChunks(ChunkManager &cm, Shader &shader, Frustum &frustum,
                                     const glm::vec3 &viewPosition, int maxRenderDistance) {
    const auto drainChunkErrors = [](const char *stage, const glm::ivec3 &chunkPos) {
        unsigned int firstError = GL_NO_ERROR;
        int count = 0;
        for (;;) {
            const unsigned int err = glGetError();
            if (err == GL_NO_ERROR) {
                break;
            }
            if (count == 0) {
                firstError = err;
            }
            ++count;
        }
        if (count > 0) {
            static int s_chunkRenderErrorLogCount = 0;
            if (s_chunkRenderErrorLogCount < 32) {
                std::cerr << "[chunk] GL error during " << stage << ": count=" << count
                          << " first=0x" << std::hex << firstError << std::dec << " chunk=("
                          << chunkPos.x << "," << chunkPos.y << "," << chunkPos.z << ")"
                          << "\n";
                ++s_chunkRenderErrorLogCount;
            }
        }
        return count;
    };

    const glm::vec3 playerPos = viewPosition;
    const glm::ivec3 playerBlockPos(static_cast<int>(std::floor(playerPos.x)),
                                    static_cast<int>(std::floor(playerPos.y)),
                                    static_cast<int>(std::floor(playerPos.z)));
    const glm::ivec3 playerChunkPos = cm.worldToChunkPos(playerBlockPos);

    size_t regionCount = 0;
    size_t validMeshCount = 0;
    size_t drawnCount = 0;
    size_t distCullCount = 0;
    size_t frustumCullCount = 0;

    for (auto &[regionPos, region] : cm.regions) {
        ++regionCount;
        glm::vec3 regionMin = glm::vec3(regionPos * REGION_SIZE * CHUNK_SIZE);
        glm::vec3 regionMax = regionMin + glm::vec3(REGION_SIZE * CHUNK_SIZE);
        if (!frustum.isBoxVisible(regionMin, regionMax)) {
            frustumCullCount += region.chunks.size();
            continue;
        }

        RegionMeshBuffer &gpu = *region.gpu;
        for (const auto &[chunkPos, mesh] : region.chunks) {
            if (!mesh.valid) {
                continue;
            }
            ++validMeshCount;

            glm::ivec3 d = chunkPos - playerChunkPos;
            // Client-side render culling uses radial distance in XZ.
            const int64_t dist2 = static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
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
            GLint currentProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
            if (static_cast<GLuint>(currentProgram) != shader.ID) {
                shader.use();
            }
            shader.setMat4("model", model);
            if (drainChunkErrors("set model uniform", chunkPos) > 0) {
                continue;
            }
            gpu.drawChunkMesh(mesh);
            ++drawnCount;
        }
    }
}

void ChunkRenderSystem::renderChunkBorders(ChunkManager &cm, glm::mat4 &view,
                                           glm::mat4 &projection) {
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

void ChunkRenderSystem::renderChunksDepthPass(ChunkManager &cm, GLuint shadowProgram,
                                              const glm::mat4 &lightViewProj,
                                              const glm::vec3 &viewPosition,
                                              int maxRenderDistance) {
    if (shadowProgram == 0) {
        return;
    }

    const glm::ivec3 playerBlockPos(static_cast<int>(std::floor(viewPosition.x)),
                                    static_cast<int>(std::floor(viewPosition.y)),
                                    static_cast<int>(std::floor(viewPosition.z)));
    const glm::ivec3 playerChunkPos = cm.worldToChunkPos(playerBlockPos);
    const int shadowCullDistance = std::max(1, maxRenderDistance + 4);
    const int64_t radius2 =
        static_cast<int64_t>(shadowCullDistance) * static_cast<int64_t>(shadowCullDistance);

    const GLint lightVpLoc = glGetUniformLocation(shadowProgram, "uLightViewProj");
    const GLint modelLoc = glGetUniformLocation(shadowProgram, "uModel");
    if (lightVpLoc < 0 || modelLoc < 0) {
        return;
    }

    glUseProgram(shadowProgram);
    glUniformMatrix4fv(lightVpLoc, 1, GL_FALSE, glm::value_ptr(lightViewProj));

    for (auto &[regionPos, region] : cm.regions) {
        (void)regionPos;
        RegionMeshBuffer &gpu = *region.gpu;
        for (const auto &[chunkPos, mesh] : region.chunks) {
            if (!mesh.valid) {
                continue;
            }
            glm::ivec3 d = chunkPos - playerChunkPos;
            const int64_t dist2 = static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
                                  static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
            if (dist2 > radius2) {
                continue;
            }

            glm::vec3 min = glm::vec3(chunkPos * CHUNK_SIZE);
            glm::mat4 model(1.0f);
            model[3] = glm::vec4(min, 1.0f);
            glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
            gpu.drawChunkMesh(mesh);
        }
    }

    glUseProgram(0);
}
