#pragma once
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
