#include "OpenGLTextureAtlas.hpp"

#include "../AtlasLayout.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <stb_image.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
    struct LoadedImage {
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<uint8_t> pixels;
    };

    bool loadImageFlipped(const char *path, LoadedImage &out) {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_set_flip_vertically_on_load(true);
        unsigned char *data = stbi_load(path, &width, &height, &channels, 0);
        if (!data) {
            std::cerr << "Failed to load texture: " << path << "\n";
            return false;
        }

        out.width = width;
        out.height = height;
        out.channels = channels;
        out.pixels.assign(
            data,
            data + (static_cast<size_t>(width) * static_cast<size_t>(height) *
                    static_cast<size_t>(channels))
        );
        stbi_image_free(data);
        return true;
    }

    bool resolveFormats(int channels, GLenum &internalFormat, GLenum &format) {
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

    GLuint createTextureArrayFromAtlasImage(const LoadedImage &atlasImage) {
        if (atlasImage.width != TEXTURE_ATLAS_SIZE * TILE_RESOLUTION ||
            atlasImage.height != TEXTURE_ATLAS_SIZE * TILE_RESOLUTION) {
            std::cerr << "Unexpected atlas dimensions: " << atlasImage.width << "x"
                      << atlasImage.height << " expected " << (TEXTURE_ATLAS_SIZE * TILE_RESOLUTION)
                      << "x" << (TEXTURE_ATLAS_SIZE * TILE_RESOLUTION) << "\n";
            return 0;
        }

        GLenum internalFormat = GL_RGBA8;
        GLenum format = GL_RGBA;
        if (!resolveFormats(atlasImage.channels, internalFormat, format)) {
            std::cerr << "Unsupported channel count for array texture: " << atlasImage.channels
                      << "\n";
            return 0;
        }

        constexpr int kLayerCount = TEXTURE_ATLAS_SIZE * TEXTURE_ATLAS_SIZE;
        const size_t tileRowBytes =
            static_cast<size_t>(TILE_RESOLUTION) * static_cast<size_t>(atlasImage.channels);
        std::vector<uint8_t> tilePixels(
            static_cast<size_t>(TILE_RESOLUTION) * static_cast<size_t>(TILE_RESOLUTION) *
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
                         static_cast<size_t>(tileX * TILE_RESOLUTION)) *
                        static_cast<size_t>(atlasImage.channels);
                    const size_t dstOffset = static_cast<size_t>(row) * tileRowBytes;
                    std::memcpy(
                        tilePixels.data() + dstOffset,
                        atlasImage.pixels.data() + srcOffset,
                        tileRowBytes
                    );
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
} // namespace

OpenGLTextureAtlas::~OpenGLTextureAtlas() {
    cleanup();
}

bool OpenGLTextureAtlas::initialize() {
    if (m_arrayTextureId != 0) {
        return true;
    }
    if (SDL_GL_GetCurrentContext() == nullptr) {
        return false;
    }

    const std::string atlasPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("assets/textures/textureAtlas.png")
            .generic_string();

    LoadedImage atlasImage;
    if (!loadImageFlipped(atlasPath.c_str(), atlasImage)) {
        return false;
    }

    m_arrayTextureId = createTextureArrayFromAtlasImage(atlasImage);
    if (m_arrayTextureId == 0) {
        std::cerr << "[OpenGL] Failed to create atlas texture array.\n";
        return false;
    }
    return true;
}

void OpenGLTextureAtlas::cleanup() {
    if (SDL_GL_GetCurrentContext() == nullptr) {
        m_arrayTextureId = 0;
        return;
    }
    if (m_arrayTextureId != 0) {
        glDeleteTextures(1, &m_arrayTextureId);
        m_arrayTextureId = 0;
    }
}
