#pragma once
#include "../voxels/Chunk.hpp"
#include "../voxels/Voxel.hpp"
#include "Mesh.hpp"
#include "../graphics/TextureAtlas.hpp"
#include "Renderer.hpp"
#include <array>




struct GreedyCell {
	bool valid = false;
	int sign = 0; // +1 or -1
	BlockID block = BlockID::Air;
	uint8_t matId = 0;
	uint8_t ao[4] = { 0,0,0,0 };
	uint8_t sun[4] = { 0,0,0,0 };
};

struct BuiltChunkMesh {
	std::vector<VoxelVertex> vertices;
	std::vector<uint16_t> indices;
};


class ChunkMeshBuilder {
public:
	Mesh buildFullChunkMesh(const Chunk& chunk, const TextureAtlas& atlas);
	Mesh buildPartialChunkMesh(
		const Chunk& chunk,
		const TextureAtlas& atlas,
		std::array<bool, 6> visibleFaces/* 0 : +X, 1 : -X, 2 : +Z, 3 : -Z, 4 : +Y, 5 : -Y */
	); //build mesh for a chunk with only some faces visible, 
	   //for example when visibleFaces is [false, false, false, false] it will only build the top 
	   //and bottom faces of the chunk





	Mesh buildPartialChunkMeshGreedy(
		const Chunk& chunk,
		const TextureAtlas& atlas,
		std::array<bool, 6> visibleFaces 
	);

	BlockID getBlockWithNeighbors(
		int x, int y, int z,
		const Chunk& chunk,
		const Chunk* neighbors[6]
	) {
		if (x >= 0 && x < CHUNK_SIZE &&
			y >= 0 && y < CHUNK_SIZE &&
			z >= 0 && z < CHUNK_SIZE)
			return chunk.getBlock(x, y, z);

		// Check neighbor chunk
		const Chunk* neighbor = nullptr;
		int nx = x, ny = y, nz = z;

		if (x < 0) { neighbor = neighbors[1]; nx += CHUNK_SIZE; }
		else if (x >= CHUNK_SIZE) { neighbor = neighbors[0]; nx -= CHUNK_SIZE; }
		else if (z < 0) { neighbor = neighbors[3]; nz += CHUNK_SIZE; }
		else if (z >= CHUNK_SIZE) { neighbor = neighbors[2]; nz -= CHUNK_SIZE; }
		else if (y < 0) { neighbor = neighbors[5]; ny += CHUNK_SIZE; }
		else if (y >= CHUNK_SIZE) { neighbor = neighbors[4]; ny -= CHUNK_SIZE; }

		if (!neighbor) return BlockID::Air;
		return neighbor->getBlock(nx, ny, nz);
	}


	using BlockGetter = std::function<BlockID(const glm::ivec3& worldPos)>;

	BuiltChunkMesh buildChunkMesh(
		const Chunk& chunk,
		const glm::ivec3& chunkPos,   // chunk coordinates (in chunks)
		const TextureAtlas& atlas,
		BlockGetter getBlock,        
		bool enableAO,
		bool enableShadows
	);


	Mesh buildGreedyMesh(const Chunk& chunk,
		const glm::ivec3& chunkPos,
		const TextureAtlas& atlas,
		BlockGetter getBlock
	);

private:











};



