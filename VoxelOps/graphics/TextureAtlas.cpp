#include "TextureAtlas.hpp"
#include "../../Shared/runtime/Paths.hpp"
#include <stb_image.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
struct LoadedImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> pixels;
};

bool loadImageFlipped(const char* path, LoadedImage& out) {
    int width = 0;
    int height = 0;
    int nrChannels = 0;

    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);
    if (!data) {
        std::cerr << "Failed to load texture: " << path << '\n';
        return false;
    }

    out.width = width;
    out.height = height;
    out.channels = nrChannels;
    out.pixels.assign(data, data + (static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(nrChannels)));
    stbi_image_free(data);
    return true;
}

bool resolveFormats(int channels, GLenum& internalFormat, GLenum& format) {
    if (channels == 1) {
        internalFormat = GL_R8;
        format = GL_RED;
        return true;
    }
    if (channels == 3) {
        internalFormat = GL_SRGB8;
        format = GL_RGB;
        return true;
    }
    if (channels == 4) {
        internalFormat = GL_SRGB8_ALPHA8;
        format = GL_RGBA;
        return true;
    }
    return false;
}

GLuint createTexture2DFromImage(const LoadedImage& image) {
    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    if (!resolveFormats(image.channels, internalFormat, format)) {
        std::cerr << "Unsupported channel count: " << image.channels << '\n';
        return 0;
    }

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        internalFormat,
        image.width,
        image.height,
        0,
        format,
        GL_UNSIGNED_BYTE,
        image.pixels.data()
    );

#if defined(GL_MAX_TEXTURE_MAX_ANISOTROPY) && defined(GL_TEXTURE_MAX_ANISOTROPY)
    GLfloat aniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
#endif

    glEnable(GL_FRAMEBUFFER_SRGB);

    glBindTexture(GL_TEXTURE_2D, 0);

    return textureID;
}

GLuint createTextureArrayFromAtlasImage(const LoadedImage& atlasImage) {
    if (atlasImage.width != TEXTURE_ATLAS_SIZE * TILE_RESOLUTION ||
        atlasImage.height != TEXTURE_ATLAS_SIZE * TILE_RESOLUTION) {
        std::cerr
            << "Unexpected atlas dimensions: "
            << atlasImage.width << "x" << atlasImage.height
            << " expected "
            << (TEXTURE_ATLAS_SIZE * TILE_RESOLUTION) << "x"
            << (TEXTURE_ATLAS_SIZE * TILE_RESOLUTION) << '\n';
        return 0;
    }

    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    if (!resolveFormats(atlasImage.channels, internalFormat, format)) {
        std::cerr << "Unsupported channel count for array texture: " << atlasImage.channels << '\n';
        return 0;
    }

    constexpr int kLayerCount = TEXTURE_ATLAS_SIZE * TEXTURE_ATLAS_SIZE;
    const size_t tileRowBytes = static_cast<size_t>(TILE_RESOLUTION) * static_cast<size_t>(atlasImage.channels);
    std::vector<uint8_t> tilePixels(
        static_cast<size_t>(TILE_RESOLUTION) *
        static_cast<size_t>(TILE_RESOLUTION) *
        static_cast<size_t>(atlasImage.channels)
    );

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textureID);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        internalFormat,
        TILE_RESOLUTION,
        TILE_RESOLUTION,
        kLayerCount,
        0,
        format,
        GL_UNSIGNED_BYTE,
        nullptr
    );

    for (int tileY = 0; tileY < TEXTURE_ATLAS_SIZE; ++tileY) {
        for (int tileX = 0; tileX < TEXTURE_ATLAS_SIZE; ++tileX) {
            const int layer = tileY * TEXTURE_ATLAS_SIZE + tileX;
            for (int row = 0; row < TILE_RESOLUTION; ++row) {
                const int srcY = tileY * TILE_RESOLUTION + row;
                const size_t srcOffset =
                    (static_cast<size_t>(srcY) * static_cast<size_t>(atlasImage.width) +
                        static_cast<size_t>(tileX * TILE_RESOLUTION)) * static_cast<size_t>(atlasImage.channels);
                const size_t dstOffset = static_cast<size_t>(row) * tileRowBytes;
                std::memcpy(tilePixels.data() + dstOffset, atlasImage.pixels.data() + srcOffset, tileRowBytes);
            }

            glTexSubImage3D(
                GL_TEXTURE_2D_ARRAY,
                0,
                0,
                0,
                layer,
                TILE_RESOLUTION,
                TILE_RESOLUTION,
                1,
                format,
                GL_UNSIGNED_BYTE,
                tilePixels.data()
            );
        }
    }

    glEnable(GL_FRAMEBUFFER_SRGB);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return textureID;
}
}

TextureAtlas::TextureAtlas(){
    const std::string atlasPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("assets/textures/textureAtlas.png").generic_string();

    LoadedImage atlasImage;
    if (!loadImageFlipped(atlasPath.c_str(), atlasImage)) {
        throw std::runtime_error("Failed to load texture atlas image data");
    }

    atlasTextureID = createTexture2DFromImage(atlasImage);
    if (!atlasTextureID) {
        throw std::runtime_error("Failed to create 2D texture atlas");
    }

    atlasTextureArrayID = createTextureArrayFromAtlasImage(atlasImage);
    if (!atlasTextureArrayID) {
        throw std::runtime_error("Failed to create texture array from atlas");
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

TextureAtlas::~TextureAtlas() {
    if (atlasTextureArrayID != 0) {
        glDeleteTextures(1, &atlasTextureArrayID);
        atlasTextureArrayID = 0;
    }
    if (atlasTextureID != 0) {
        glDeleteTextures(1, &atlasTextureID);
        atlasTextureID = 0;
    }
}


