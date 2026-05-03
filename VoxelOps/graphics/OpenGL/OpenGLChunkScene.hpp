#pragma once

#include "../../render/ChunkMeshData.hpp"
#include "../ChunkRegionConfig.hpp"
#include "../../voxels/Chunk.hpp"
#include "../../voxels/VoxelCoordHash.hpp"
#include "Shader.hpp"
#include "OpenGLChunkRenderCache.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

class Frustum;

class OpenGLChunkScene {
public:
    OpenGLChunkScene();
    ~OpenGLChunkScene();

    void syncFromCpuChunkMeshes(
        const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes
    );
    void renderChunks(
        Shader &shader, Frustum &frustum, const glm::vec3 &viewPosition, int maxRenderDistance
    );
    void renderChunkBorders(const glm::mat4 &view, const glm::mat4 &projection);

private:
    OpenGLChunkRenderCache m_chunkCache;
    unsigned int m_wireVAO = 0;
    unsigned int m_wireVBO = 0;
    std::optional<Shader> m_debugShader;
};
