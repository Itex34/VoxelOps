#include "TextureAtlas.hpp"
#include <stb_image.h>

#include <iostream>

namespace {
GLuint loadTexture2DNearestSRGB(const char* path) {
    int width = 0;
    int height = 0;
    int nrChannels = 0;

    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);
    if (!data) {
        std::cerr << "Failed to load texture: " << path << '\n';
        return 0;
    }

    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    if (nrChannels == 1) {
        internalFormat = GL_R8;
        format = GL_RED;
    }
    else if (nrChannels == 3) {
        internalFormat = GL_SRGB8;
        format = GL_RGB;
    }
    else if (nrChannels == 4) {
        internalFormat = GL_SRGB8_ALPHA8;
        format = GL_RGBA;
    }
    else {
        std::cerr << "Unsupported channel count: " << nrChannels << '\n';
        stbi_image_free(data);
        return 0;
    }

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);

#if defined(GL_MAX_TEXTURE_MAX_ANISOTROPY) && defined(GL_TEXTURE_MAX_ANISOTROPY)
    GLfloat aniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
#endif

    glEnable(GL_FRAMEBUFFER_SRGB);

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);

    return textureID;
}
}

TextureAtlas::TextureAtlas(){
    atlasTextureID = loadTexture2DNearestSRGB("../../../../VoxelOps/assets/textures/textureAtlas.png");
    if (!atlasTextureID) {
        throw std::runtime_error("Failed to load texture atlas");
    }

    tileMap["dirt"] = { 0, 0 };
    tileMap["grass_side"] = { 1, 0 };
    tileMap["grass_top"] = { 2, 0 };
    tileMap["stone"] = { 1, 1 };
	tileMap["bedrock"] = { 2, 1 };
	tileMap["sand"] = { 3, 0 };
	tileMap["log_side"] = { 4, 0 };
	tileMap["log_top"] = { 5, 0 };
	tileMap["stone_brick"] = { 6, 0 };
	tileMap["temple_brick"] = { 3, 1 };
	tileMap["wood"] = { 7, 0 };
	tileMap["leaves"] = { 0, 1 };
	tileMap["iron_ore"] = { 1, 3 };
	tileMap["iron_block"] = { 3, 2 };
	tileMap["emerald_ore"] = { 4, 2 };
	tileMap["red_berry"] = { 3, 6 };
	tileMap["orange_berry"] = { 4, 6 };
	tileMap["ruby_gem"] = { 0, 3 };
    tileMap["sapphire_gem"] = { 5, 2 };
    tileMap["crafting_table_top"] = { 4, 4 };
    tileMap["crafting_table_bottom"] = { 2, 2 };
    tileMap["crafting_table_rl_side"] = { 3, 4 };
    tileMap["crafting_table_fb_side"] = { 5, 4 };
    tileMap["bomb_top"] = { 7, 7 };
    tileMap["bomb_bottom"] = { 7, 6 };
    tileMap["bomb_side"] = { 6, 7 };
    tileMap["cactus_top"] = { 2, 3 };
	tileMap["cactus_bottom"] = { 3, 3 };
    tileMap["cactus_side"] = { 4, 3 };
    tileMap["ruby_block"] = { 5, 6 };
    tileMap["sapphire_block"] = { 6, 6 };
}


std::pair<glm::vec2, glm::vec2> TextureAtlas::getUVRect(const std::string& name) const {
    auto it = tileMap.find(name);
    if (it == tileMap.end()) {
        throw std::runtime_error("Tile not found in atlas: " + name);
    }

    glm::ivec2 tilePos = it->second;


    float tileWidth = 1.0f / static_cast<float>(TEXTURE_ATLAS_SIZE);
    float tileHeight = 1.0f / static_cast<float>(TEXTURE_ATLAS_SIZE);

    glm::vec2 topLeft = glm::vec2(tilePos.x * tileWidth, tilePos.y * tileHeight);
    glm::vec2 bottomRight = topLeft + glm::vec2(tileWidth, tileHeight);

    return { topLeft, bottomRight };
}


