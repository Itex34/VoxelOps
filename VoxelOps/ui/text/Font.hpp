#pragma once

#include <glm/glm.hpp>
#include <unordered_map>


using FontHandle = uint32_t;
using TextureHandle = uintptr_t;

struct Glyph {
    glm::vec2 uvMin;
    glm::vec2 uvMax;

    glm::ivec2 size;    // bitmap size
    glm::ivec2 bearing; // offset from baseline
    uint32_t advance;   // usually in pixels
};

class Font {
public:
    const Glyph &getGlyph(char c) const;
    TextureHandle getAtlasTexture() const;

private:
    TextureHandle m_atlasTexture{};
    std::unordered_map<char, Glyph> m_glyphs;
    float m_pixelSize = 0.0f;

    friend class FontManager;
};