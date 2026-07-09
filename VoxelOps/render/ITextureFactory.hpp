#pragma once
#include <cstdint>

#include "../graphics/TextureHandle.hpp"

using TextureFormat = uint32_t;
inline constexpr TextureFormat kTextureFormatR8 = 1;
inline constexpr TextureFormat kTextureFormatRgba8 = 2;

class ITextureFactory {
public:
    virtual ~ITextureFactory() = default;

    virtual TextureHandle
    createTexture2D(int width, int height, const void *pixels, TextureFormat format) = 0;
};
