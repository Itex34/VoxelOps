#include "Voxel.hpp"


const glm::ivec3 faceNormals[6] = {
    {-1, 0, 0}, // -X
    { 1, 0, 0}, // +X
    { 0,-1, 0}, // -Y
    { 0, 1, 0}, // +Y
    { 0, 0,-1}, // -Z
    { 0, 0, 1}  // +Z
};


const glm::ivec3 faceVertices[6][4] = {
    // -X
    {
        {0, 0, 1},
        {0, 1, 1},
        {0, 1, 0},
        {0, 0, 0}
    },

    // +X
    {
        {1, 0, 0},
        {1, 1, 0},
        {1, 1, 1},
        {1, 0, 1}
    },

    // -Y
    {
        {0, 0, 0},
        {1, 0, 0},
        {1, 0, 1},
        {0, 0, 1}
    },

    // +Y
    {
        {0, 1, 1},
        {1, 1, 1},
        {1, 1, 0},
        {0, 1, 0}
    },

    // -Z
    {
        {1, 0, 0},
        {0, 0, 0},
        {0, 1, 0},
        {1, 1, 0}
    },

    // +Z
    {
        {0, 0, 1},
        {1, 0, 1},
        {1, 1, 1},
        {0, 1, 1}
    }
};



const glm::ivec3 faceAxisU[6] = {
    // -X, +X
    { 0, 0, -1 }, { 0, 0,  1 },

    // -Y, +Y
    { 1, 0,  0 }, { 1, 0,  0 },

    // -Z, +Z
    { 1, 0,  0 }, { -1, 0,  0 }
};

const glm::ivec3 faceAxisV[6] = {
    // -X, +X
    { 0, 1,  0 }, { 0, 1,  0 },

    // -Y, +Y 
    { 0, 0,  1 }, { 0, 0,  1 },

    // -Z, +Z
    { 0, 1,  0 }, { 0, 1,  0 }
};











const int FACE_CORNER_SIGNS[6][4][2] = {
    // ------------------------------------------------
    // Face 0: -X
    // ------------------------------------------------
    { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} },

    // ------------------------------------------------
    // Face 1: +X
    // ------------------------------------------------
    { {+1,-1}, {-1,-1}, {-1,+1}, {+1,+1} },

    // ------------------------------------------------
    // Face 2: -Y
    // ------------------------------------------------
    { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} },

    // ------------------------------------------------
    // Face 3: +Y
    // ------------------------------------------------
    { {-1,+1}, {+1,+1}, {+1,-1}, {-1,-1} },

    // ------------------------------------------------
    // Face 4: -Z
    // ------------------------------------------------
    { {+1,-1}, {-1,-1}, {-1,+1}, {+1,+1} },

    // ------------------------------------------------
    // Face 5: +Z
    // ------------------------------------------------
    { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} }
};



const std::array<glm::vec2, 4> baseUVs = {
    glm::vec2(0.0f, 0.0f),
    glm::vec2(1.0f, 0.0f),
    glm::vec2(1.0f, 1.0f),
    glm::vec2(0.0f, 1.0f)
};




const uint8_t uvRemap[6][4] = {
    // +X
    { 1, 2, 3, 0 },
    // -X
    { 0, 3, 2, 1 },

    // +Y
    { 0, 1, 2, 3 },
    // -Y
    { 3, 2, 1, 0 },

    // +Z
    { 0, 1, 2, 3 },
    // -Z
    { 1, 0, 3, 2 }
};



std::array<glm::vec2, 4> getTexCoordsForFace(BlockID blockID, int face, const TextureAtlas& atlas) {
    const auto& block = blockTypes.at(blockID);

    std::string tileName;
    switch (face) {
        case 0: case 1: tileName = block.textures.RLSide; break;
        case 4: case 5: tileName = block.textures.FBSide; break;
        case 2: tileName = block.textures.top; break;
        case 3: tileName = block.textures.bottom; break;
    }

    auto [uvTopLeft, uvBottomRight] = atlas.getUVRect(tileName);

    std::array<glm::vec2, 4> uvs = {
        glm::vec2(uvTopLeft.x, uvTopLeft.y),
        glm::vec2(uvBottomRight.x, uvTopLeft.y),
        glm::vec2(uvBottomRight.x, uvBottomRight.y),
        glm::vec2(uvTopLeft.x, uvBottomRight.y)
    };

    return uvs;
}

