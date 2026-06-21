#pragma once
#include <cstdint>

using TextureHandle = std::intptr_t;
using TextureFormat = std::uint32_t;

class ITextureFactory {
public:
    virtual TextureHandle
    createTexture2D(int width, int height, const void *pixels, TextureFormat format) = 0;
};