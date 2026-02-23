#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

class ChunkManager;
class Shader;
class Frustum;
class Player;

class ChunkRenderSystem {
public:
    static void renderChunks(
        ChunkManager& cm,
        Shader& shader,
        Frustum& frustum,
        Player& player,
        int maxRenderDistance
    );

    static void renderChunkBorders(
        ChunkManager& cm,
        glm::mat4& view,
        glm::mat4& projection
    );
};





struct DrawElementsIndirectCommand {
    GLuint  count;
    GLuint  instanceCount;
    GLuint  firstIndex;
    GLuint  baseVertex;
    GLuint  baseInstance;
};