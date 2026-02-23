#pragma once
#include <string>
#include <glm/glm.hpp>
#include <stdexcept>
#include <unordered_map>
#include <glad/glad.h>

constexpr int TEXTURE_ATLAS_SIZE = 16; // 16x16 blocks in the atlas
constexpr float ATLAS_TILE_SIZE = 1.0f / TEXTURE_ATLAS_SIZE;

struct TextureAtlas {
public:
	TextureAtlas();

	std::unordered_map<std::string, glm::ivec2> tileMap;

	std::pair<glm::vec2, glm::vec2>  getUVRect(const std::string& tileName) const;

	GLuint atlasTextureID;

};
