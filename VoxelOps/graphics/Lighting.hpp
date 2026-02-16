// Lighting.hpp
#pragma once

#include <vector>
#include <functional>
#include <cmath>  
#include <algorithm>

#include <glm/vec3.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "../voxels/Chunk.hpp"
#include "../voxels/ChunkColumn.hpp"



static constexpr int PAD = 1;


using BlockGetter = std::function<BlockID(const glm::ivec3&)>;
using TopOccluderGetter = std::function<int(int, int)>;

class Lighting {
public:
    Lighting(int chunkSize = CHUNK_SIZE);





    void prepareChunkAO(
        const Chunk& chunk,
        const glm::ivec3& chunkPos,
        const Chunk* neighbors[6],
        std::vector<uint8_t>& aoBuffer
    );


    void prepareChunkSunlight(
        const Chunk& chunk,
        const glm::ivec3& chunkPos,
        const Chunk* neighbors[6],
        std::vector<uint8_t>& sunlightBuffer,
        float sunFalloff, // how quickly light dims below occluders
        const TopOccluderGetter& getTopOccluderY = TopOccluderGetter{}
    ) ;


    void faceCornerIndicesForCell(
        int sx, int sy, int sz,   // sampling cell base (see notes below)
        int face,                 // 0..5 (same enum as your mesher)
        int outIdx[4]             // returns 4 corner indices in BL, BR, TR, TL order
    ) const;


    inline int cornerIndexPadded(int x, int y, int z) const
    {
        x += PAD;
        y += PAD;
        z += PAD;

        return x + paddedSize * (y + paddedSize * z);
    }

private:
    int chunkSize;
    int chunkSizePlus1;
    int paddedSize;
    static constexpr float AO_TABLE[4] = { 1.00f, 0.85f, 0.65f, 0.53f };
    static const glm::ivec3 CANONICAL_CORNER_OFF[3];


    static constexpr float SUN_FALLOFF_TABLE[5] = {
        1.0f,       // 0 blockers
        0.85f,      // 1 blocker
        0.7225f,    // 2 blockers
        0.614125f,  // 3 blockers
        0.52200625f // 4 blockers
    };

    inline BlockID getBlockWithNeighbors(const glm::ivec3& pos, const Chunk& chunk, const Chunk* neighbors[6]) const noexcept
    {
        int x = pos.x;
        int y = pos.y;
        int z = pos.z;

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




    inline bool isSolidSafePadded(int x, int y, int z,
        const Chunk& center,
        const Chunk* neighbors[6])
    {
        if (x >= 0 && x < CHUNK_SIZE &&
            y >= 0 && y < CHUNK_SIZE &&
            z >= 0 && z < CHUNK_SIZE)
            return center.getBlock(x, y, z) != BlockID::Air;

        if (x < 0 && neighbors[1]) return neighbors[1]->getBlock(x + CHUNK_SIZE, y, z) != BlockID::Air;
        if (x >= CHUNK_SIZE && neighbors[0]) return neighbors[0]->getBlock(x - CHUNK_SIZE, y, z) != BlockID::Air;
        if (y < 0 && neighbors[3]) return neighbors[3]->getBlock(x, y + CHUNK_SIZE, z) != BlockID::Air;
        if (y >= CHUNK_SIZE && neighbors[2]) return neighbors[2]->getBlock(x, y - CHUNK_SIZE, z) != BlockID::Air;
        if (z < 0 && neighbors[5]) return neighbors[5]->getBlock(x, y, z + CHUNK_SIZE) != BlockID::Air;
        if (z >= CHUNK_SIZE && neighbors[4]) return neighbors[4]->getBlock(x, y, z - CHUNK_SIZE) != BlockID::Air;

        return false;
    }




};
