#pragma once
#include "../voxels/Chunk.hpp"
#include "../voxels/Voxel.hpp"
#include "Mesh.hpp"
#include "../graphics/TextureAtlas.hpp"
#include "Renderer.hpp"
#include <array>



// --- put these at top of function (static across planes) ---
struct QuadKey {
    uint32_t x0, y0, z0;
    uint32_t x1, y1, z1;
    uint32_t x2, y2, z2;
    uint32_t x3, y3, z3;
    uint8_t axis;
    int8_t sign;

    bool operator==(QuadKey const&) const = default;
};

struct QuadKeyHash {
    size_t operator()(QuadKey const& k) const noexcept {
        // cheap XOR hash
        uint64_t a = ((uint64_t)k.x0 << 32) ^ (k.y0 << 16) ^ k.z0;
        uint64_t b = ((uint64_t)k.x1 << 32) ^ (k.y1 << 16) ^ k.z1;
        uint64_t c = ((uint64_t)k.x2 << 32) ^ (k.y2 << 16) ^ k.z2;
        uint64_t d = ((uint64_t)k.x3 << 32) ^ (k.y3 << 16) ^ k.z3;
        return (size_t)(a ^ (b << 1) ^ (c << 2) ^ (d << 3));
    }
};
static



struct GreedyCell {
	bool valid = false;
	int sign = 0; // +1 or -1
	BlockID block = BlockID::Air;
	uint8_t matId = 0;
    uint8_t ao[4] = { 15,15,15,15 };
    uint8_t sun[4] = { 15,15,15,15 };
    uint32_t lightKey;
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




	using BlockGetter = std::function<BlockID(const glm::ivec3& worldPos)>; //deprecated

    BuiltChunkMesh buildChunkMesh(
        const Chunk& center,
        const Chunk* neighbors[6],
        const glm::ivec3& chunkPos,
        const TextureAtlas& atlas,
        bool enableAO,
        bool enableShadows
    );



	Mesh buildGreedyMesh(const Chunk& chunk,
		const glm::ivec3& chunkPos,
		const TextureAtlas& atlas,
		BlockGetter getBlock
	);

private:
    std::unordered_map<QuadKey, uint16_t, QuadKeyHash> quadEmitMap;
    std::unordered_map<QuadKey, std::pair<std::array<uint8_t, 4>, std::array<uint8_t, 4>>, QuadKeyHash> quadMap;

    inline BlockID getBlockWithNeighbors(
        int x, int y, int z,
        const Chunk& chunk,
        const Chunk* neighbors[6]
    ) noexcept
    {
        // Fast interior path
        if ((unsigned)x < CHUNK_SIZE &&
            (unsigned)y < CHUNK_SIZE &&
            (unsigned)z < CHUNK_SIZE)
        {
            return chunk.getBlockUnchecked(x, y, z);
        }

        // Only ONE axis can be out of bounds in meshing
        if (x < 0) {
            const Chunk* n = neighbors[1]; // -X
            return n ? n->getBlockUnchecked(x + CHUNK_SIZE, y, z) : BlockID::Air;
        }
        if (x >= CHUNK_SIZE) {
            const Chunk* n = neighbors[0]; // +X
            return n ? n->getBlockUnchecked(x - CHUNK_SIZE, y, z) : BlockID::Air;
        }
        if (y < 0) {
            const Chunk* n = neighbors[3]; // -Y
            return n ? n->getBlockUnchecked(x, y + CHUNK_SIZE, z) : BlockID::Air;
        }
        if (y >= CHUNK_SIZE) {
            const Chunk* n = neighbors[2]; // +Y
            return n ? n->getBlockUnchecked(x, y - CHUNK_SIZE, z) : BlockID::Air;
        }
        if (z < 0) {
            const Chunk* n = neighbors[5]; // -Z
            return n ? n->getBlockUnchecked(x, y, z + CHUNK_SIZE) : BlockID::Air;
        }
        if (z >= CHUNK_SIZE) {
            const Chunk* n = neighbors[4]; // +Z
            return n ? n->getBlockUnchecked(x, y, z - CHUNK_SIZE) : BlockID::Air;
        }

        return BlockID::Air;
    }




    inline BlockID getBlockSafe(
        int x, int y, int z,
        const Chunk& chunk,
        const Chunk* neighbors[6]
    ) noexcept
    {
        // Hard reject unsupported vertical access
        if (y < -1 || y > CHUNK_SIZE)
            return BlockID::Air;

        int oob =
            (x < 0 || x >= CHUNK_SIZE) +
            (y < 0 || y >= CHUNK_SIZE) +
            (z < 0 || z >= CHUNK_SIZE);

        if (oob == 0)
            return chunk.getBlockUnchecked(x, y, z);

        if (oob == 1)
            return getBlockWithNeighbors( x, y, z , chunk, neighbors);

        return BlockID::Air;
    }







};



