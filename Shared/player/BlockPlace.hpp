#pragma once
#include <glm/glm.hpp>
#include <array>


namespace BlockPlace {
enum class BlockMode {
	Block = 0,
	Wall,
	Stair,
	Floor,
	COUNT
};

//the different block modes are just collections of full blocks
struct BlockPatch {
	glm::vec3 offset;
	BlockID type;

};


struct PlaceResult {

	std::array<uint8_t, 6> chunkNormalsToCenterBlock;
	glm::vec3()
};

}