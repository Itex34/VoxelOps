#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

class ChunkManager;
class Shader;
class Frustum;

class ChunkRenderSystem {
public:
    static void renderChunks(
        ChunkManager& cm,
        Shader& shader,
        Frustum& frustum,
        const glm::vec3& viewPosition,
        int maxRenderDistance
    );

    static void renderChunkBorders(
        ChunkManager& cm,
        glm::mat4& view,
        glm::mat4& projection
    );

    static void renderChunksDepthPass(
        ChunkManager& cm,
        GLuint shadowProgram,
        const glm::mat4& lightViewProj,
        const glm::vec3& viewPosition,
        int maxRenderDistance
    );
};





struct DrawElementsIndirectCommand {
    GLuint  count;
    GLuint  instanceCount;
    GLuint  firstIndex;
    GLuint  baseVertex;
    GLuint  baseInstance;
};
