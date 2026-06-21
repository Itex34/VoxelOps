#pragma once
#include "Font.hpp"

#include <string>
#include <filesystem>
#include <vector>

class FontManager {
public:
    FontHandle
    loadFont(const std::string &name, const std::filesystem::path &path, float pixelSize);

    const Font &getFont(FontHandle handle) const;

private:
    std::vector<Font> m_fonts;
};