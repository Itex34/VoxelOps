#include "FontManager.hpp"

#include <freetype/freetype.h>
#include <ostream>
#include <stdexcept>



FontManager::FontManager() {
    if (FT_Init_FreeType(&m_ft)) {
        throw std::runtime_error("Failed to initialize FreeType");
    }
}

FontManager::~FontManager() {
    if (m_ft) {
        FT_Done_FreeType(m_ft);
        m_ft = nullptr;
    }
}



FontHandle FontManager::loadFont(const std::string &name, const std::filesystem::path &path, float pixelSize) {
    FT_Face face = nullptr;


    
    if (auto err = FT_New_Face(m_ft, path.string().c_str(), 0, &face); err != FT_Err_Ok) {
        throw std::runtime_error(
            "Failed to load font: " + path.string() + " (error " + std::to_string(err) + ")"
        );
    }

        
    if (auto err = FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) ; err != FT_Err_Ok) {
        FT_Done_Face(face);
        throw std::runtime_error(
            "Failed to set font pixel size (error " + std::to_string(err) + ")"
        );
    }


    Font font; 
    font.name = name;
    font.pixelSize = pixelSize;


    // Basic ASCII range.
    // TODO : expand this to Unicode codepoints.
    for (char32_t codepoint = 32; codepoint < 128; ++codepoint) {
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) {
            continue;
        }

        FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap &bitmap = slot->bitmap;

        GlyphBitmap glyph;
        glyph.width = static_cast<int>(bitmap.width);
        glyph.height = static_cast<int>(bitmap.rows);
        glyph.bearingX = slot->bitmap_left;
        glyph.bearingY = slot->bitmap_top;
        glyph.advanceX = static_cast<int>(slot->advance.x);

        glyph.pixels.resize(glyph.width * glyph.height);

        for (int y = 0; y < glyph.height; ++y) {
            for (int x = 0; x < glyph.width; ++x) {
                glyph.pixels[y * glyph.width + x] = bitmap.buffer[y * bitmap.pitch + x];
            }
        }

        font.glyphs.emplace(codepoint, std::move(glyph));
    }

    FT_Done_Face(face);

    FontHandle handle = static_cast<FontHandle>(m_fonts.size());
    m_fonts.push_back(std::move(font));

    return handle;
}