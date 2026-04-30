#include "OpenGLChunkScene.hpp"

#include "RegionMeshBuffer.hpp"
#include "../Frustum.hpp"
#include "Shader.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
// positions only cube used for wireframe debug
float kCubeVertices[] = {
    0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0,

    0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1,

    0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1,
};

int floorDivLocal(int a, int b) {
    int q = a / b;
    int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        q--;
    }
    return q;
}
} // namespace

OpenGLChunkScene::OpenGLChunkScene() {
    glGenVertexArrays(1, &m_wireVAO);
    glGenBuffers(1, &m_wireVBO);
    glBindVertexArray(m_wireVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_wireVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);

    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();
    m_debugShader.emplace(debugVertPath.c_str(), debugFragPath.c_str());
}

OpenGLChunkScene::~OpenGLChunkScene() {
    if (m_wireVBO != 0) {
        glDeleteBuffers(1, &m_wireVBO);
        m_wireVBO = 0;
    }
    if (m_wireVAO != 0) {
        glDeleteVertexArrays(1, &m_wireVAO);
        m_wireVAO = 0;
    }
}

void OpenGLChunkScene::syncFromChunkManager(const ChunkManager &chunkManager) {
    m_chunkCache.syncFromChunkManager(chunkManager);
}

void OpenGLChunkScene::renderChunks(Shader &shader, Frustum &frustum, const glm::vec3 &viewPosition,
                                    int maxRenderDistance) {
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
                          << chunkPos.x << "," << chunkPos.y << "," << chunkPos.z << ")\n";
                ++s_chunkRenderErrorLogCount;
            }
        }
        return count;
    };

    const glm::ivec3 playerBlockPos(static_cast<int>(std::floor(viewPosition.x)),
                                    static_cast<int>(std::floor(viewPosition.y)),
                                    static_cast<int>(std::floor(viewPosition.z)));
    const glm::ivec3 playerChunkPos(
        floorDivLocal(playerBlockPos.x, CHUNK_SIZE), floorDivLocal(playerBlockPos.y, CHUNK_SIZE),
        floorDivLocal(playerBlockPos.z, CHUNK_SIZE));

    for (auto &[regionPos, region] : m_chunkCache.regions()) {
        glm::vec3 regionMin = glm::vec3(regionPos * REGION_SIZE * CHUNK_SIZE);
        glm::vec3 regionMax = regionMin + glm::vec3(REGION_SIZE * CHUNK_SIZE);
        if (!frustum.isBoxVisible(regionMin, regionMax)) {
            continue;
        }

        RegionMeshBuffer &gpu = *region.gpu;
        for (const auto &[chunkPos, regionMesh] : region.chunks) {
            const ChunkMesh &mesh = regionMesh.mesh;
            if (!mesh.valid) {
                continue;
            }

            glm::ivec3 d = chunkPos - playerChunkPos;
            const int64_t dist2 = static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
                                  static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
            const int64_t radius2 =
                static_cast<int64_t>(maxRenderDistance) * static_cast<int64_t>(maxRenderDistance);
            if (dist2 > radius2) {
                continue;
            }

            glm::vec3 min = glm::vec3(chunkPos * CHUNK_SIZE);
            glm::vec3 max = min + glm::vec3(CHUNK_SIZE);
            if (!frustum.isBoxVisible(min, max)) {
                continue;
            }

            glm::mat4 model(1.0f);
            model[3] = glm::vec4(min, 1.0f);
            GLint currentProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
            if (static_cast<unsigned int>(currentProgram) != shader.ID) {
                shader.use();
            }
            shader.setMat4("model", model);
            if (drainChunkErrors("set model uniform", chunkPos) > 0) {
                continue;
            }
            gpu.drawChunkMesh(mesh);
        }
    }
}

void OpenGLChunkScene::renderChunksDepthPass(uint32_t shadowProgram, const glm::mat4 &lightViewProj,
                                             const glm::vec3 &viewPosition,
                                             int maxRenderDistance) {
    if (shadowProgram == 0) {
        return;
    }

    const glm::ivec3 playerBlockPos(static_cast<int>(std::floor(viewPosition.x)),
                                    static_cast<int>(std::floor(viewPosition.y)),
                                    static_cast<int>(std::floor(viewPosition.z)));
    const glm::ivec3 playerChunkPos(
        floorDivLocal(playerBlockPos.x, CHUNK_SIZE), floorDivLocal(playerBlockPos.y, CHUNK_SIZE),
        floorDivLocal(playerBlockPos.z, CHUNK_SIZE));
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

    for (auto &[regionPos, region] : m_chunkCache.regions()) {
        (void)regionPos;
        RegionMeshBuffer &gpu = *region.gpu;
        for (const auto &[chunkPos, regionMesh] : region.chunks) {
            const ChunkMesh &mesh = regionMesh.mesh;
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

void OpenGLChunkScene::renderChunkBorders(const glm::mat4 &view, const glm::mat4 &projection) {
    if (!m_debugShader.has_value()) {
        return;
    }

    m_debugShader->use();
    m_debugShader->setMat4("projection", projection);
    m_debugShader->setMat4("view", view);
    m_debugShader->setVec3("color", glm::vec3(0.0f, 1.0f, 0.0f));

    glBindVertexArray(m_wireVAO);

    for (int z = WORLD_MIN_Z; z <= WORLD_MAX_Z; ++z) {
        for (int x = WORLD_MIN_X; x <= WORLD_MAX_X; ++x) {
            glm::ivec3 pos(x, 0, z);
            glm::vec3 worldPos = glm::vec3(pos.x * CHUNK_SIZE, 0.0f, pos.z * CHUNK_SIZE);
            glm::vec3 scale = glm::vec3(CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
            model = glm::scale(model, scale);
            m_debugShader->setMat4("model", model);
            glDrawArrays(GL_LINES, 0, 24);
        }
    }
}
