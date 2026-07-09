#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../graphics/TextureHandle.hpp"

using FontHandle = std::uint32_t;

struct GlyphBitmap {
    int width = 0;
    int height = 0;

    int bearingX = 0;
    int bearingY = 0;

    // FreeType advance is 26.6 fixed-point.
    // Divide by 64 when converting to pixels.
    int advanceX = 0;

    int atlasX = 0;
    int atlasY = 0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;

    // 8-bit grayscale alpha mask.
    // Size = width * height.
    std::vector<std::uint8_t> pixels;
};

struct Font {
    std::string name;
    float pixelSize = 0.0f;
    int ascender = 0;
    int descender = 0;
    int lineHeight = 0;
    int atlasWidth = 0;
    int atlasHeight = 0;
    TextureHandle atlasTexture = 0;

    std::unordered_map<char32_t, GlyphBitmap> glyphs;
};
