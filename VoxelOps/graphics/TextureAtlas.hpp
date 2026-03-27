#pragma once
#include <string>
#include <glm/glm.hpp>
#include <stdexcept>
#include <unordered_map>
#include <glad/glad.h>

constexpr int TEXTURE_ATLAS_SIZE = 16; // 16x16 blocks in the atlas
constexpr int TILE_RESOLUTION = 16; // each tile is 16x16 pixels
struct TextureAtlas {
public:
	TextureAtlas();
	~TextureAtlas();
	TextureAtlas(const TextureAtlas&) = delete;
	TextureAtlas& operator=(const TextureAtlas&) = delete;
	TextureAtlas(TextureAtlas&&) = delete;
	TextureAtlas& operator=(TextureAtlas&&) = delete;

	std::unordered_map<std::string, glm::ivec2> tileMap;

	GLuint atlasTextureID = 0;
	GLuint atlasTextureArrayID = 0;

};
