#include "FontAtlasBuilder.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr int kFirstGlyph = 32;
    constexpr int kLastGlyphExclusive = 128;
    constexpr int kAtlasPadding = 1;
    constexpr int kMaxAtlasWidth = 1024;

    int NextPowerOfTwo(int value) {
        int result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

    std::runtime_error FreeTypeError(const std::string &message, FT_Error error) {
        return std::runtime_error(message + " (error " + std::to_string(error) + ")");
    }
}

BuiltFontAtlas FontAtlasBuilder::buildFromFile(const std::filesystem::path &path, uint32_t pixelSize) {
    FT_Library ft = nullptr;
    if (const FT_Error err = FT_Init_FreeType(&ft); err != FT_Err_Ok) {
        throw FreeTypeError("Failed to initialize FreeType", err);
    }

    FT_Face face = nullptr;
    if (const FT_Error err = FT_New_Face(ft, path.string().c_str(), 0, &face); err != FT_Err_Ok) {
        FT_Done_FreeType(ft);
        throw FreeTypeError("Failed to load font: " + path.string(), err);
    }

    if (const FT_Error err = FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize));
        err != FT_Err_Ok) {
        FT_Done_Face(face);
        FT_Done_FreeType(ft);
        throw FreeTypeError("Failed to set font pixel size", err);
    }

    Font font;
    font.name = path.stem().string();
    font.pixelSize = static_cast<float>(pixelSize);
    font.ascender = static_cast<int>(face->size->metrics.ascender >> 6);
    font.descender = static_cast<int>(face->size->metrics.descender >> 6);
    font.lineHeight = static_cast<int>(face->size->metrics.height >> 6);

    int penX = kAtlasPadding;
    int penY = kAtlasPadding;
    int rowHeight = 0;
    int usedWidth = 0;
    int usedHeight = 0;

    for (char32_t codepoint = kFirstGlyph; codepoint < kLastGlyphExclusive; ++codepoint) {
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != FT_Err_Ok) {
            continue;
        }

        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap &bitmap = slot->bitmap;
        const int glyphWidth = static_cast<int>(bitmap.width);
        const int glyphHeight = static_cast<int>(bitmap.rows);

        if (penX + glyphWidth + kAtlasPadding > kMaxAtlasWidth) {
            penX = kAtlasPadding;
            penY += rowHeight + kAtlasPadding;
            rowHeight = 0;
        }

        GlyphBitmap glyph;
        glyph.width = glyphWidth;
        glyph.height = glyphHeight;
        glyph.bearingX = slot->bitmap_left;
        glyph.bearingY = slot->bitmap_top;
        glyph.advanceX = static_cast<int>(slot->advance.x);
        glyph.atlasX = penX;
        glyph.atlasY = penY;
        glyph.pixels.resize(static_cast<size_t>(glyphWidth * glyphHeight));

        for (int y = 0; y < glyphHeight; ++y) {
            const std::uint8_t *src = bitmap.buffer + y * bitmap.pitch;
            for (int x = 0; x < glyphWidth; ++x) {
                glyph.pixels[static_cast<size_t>(y * glyphWidth + x)] = src[x];
            }
        }

        usedWidth = std::max(usedWidth, penX + glyphWidth + kAtlasPadding);
        usedHeight = std::max(usedHeight, penY + glyphHeight + kAtlasPadding);
        rowHeight = std::max(rowHeight, glyphHeight);
        penX += glyphWidth + kAtlasPadding;

        font.glyphs.emplace(codepoint, std::move(glyph));
    }

    font.atlasWidth = NextPowerOfTwo(std::max(usedWidth, 4));
    font.atlasHeight = NextPowerOfTwo(std::max(usedHeight, 4));

    std::vector<std::uint8_t> atlas(static_cast<size_t>(font.atlasWidth * font.atlasHeight), 0);
    for (auto &[_, glyph] : font.glyphs) {
        for (int y = 0; y < glyph.height; ++y) {
            const size_t srcOffset = static_cast<size_t>(y * glyph.width);
            const size_t dstOffset =
                static_cast<size_t>((glyph.atlasY + y) * font.atlasWidth + glyph.atlasX);
            std::copy_n(glyph.pixels.data() + srcOffset, static_cast<size_t>(glyph.width), atlas.data() + dstOffset);
        }

        glyph.u0 = static_cast<float>(glyph.atlasX) / static_cast<float>(font.atlasWidth);
        glyph.v0 = static_cast<float>(glyph.atlasY) / static_cast<float>(font.atlasHeight);
        glyph.u1 = static_cast<float>(glyph.atlasX + glyph.width) / static_cast<float>(font.atlasWidth);
        glyph.v1 = static_cast<float>(glyph.atlasY + glyph.height) / static_cast<float>(font.atlasHeight);
        glyph.pixels.clear();
        glyph.pixels.shrink_to_fit();
    }

    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    return BuiltFontAtlas{std::move(font), std::move(atlas)};
}
