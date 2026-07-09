#pragma once

#include "Font.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

class FontManager {
public:
    FontManager();
    ~FontManager();

    FontManager(const FontManager &) = delete;
    FontManager &operator=(const FontManager &) = delete;

    FontHandle
    loadFont(const std::string &name, const std::filesystem::path &path, float pixelSize);

    const Font &getFont(FontHandle handle) const;

private:
    FT_Library m_ft = nullptr;
    std::vector<Font> m_fonts;
};