#pragma once

#include "Font.hpp"

#include <filesystem>
#include <vector>

struct BuiltFontAtlas {
    Font font;
    std::vector<std::uint8_t> pixelsR8;
};

class FontAtlasBuilder {
public:
    static BuiltFontAtlas buildFromFile(const std::filesystem::path &path, uint32_t pixelSize);
};
