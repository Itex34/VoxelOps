#pragma once

#include "Font.hpp"
#include "../../render/ITextureFactory.hpp"


#include <filesystem>

class FontAtlasBuilder {
public:
    static Font buildFromFile(
        const std::filesystem::path &path, uint32_t pixelSize, ITextureFactory &textureFactory
    );
};