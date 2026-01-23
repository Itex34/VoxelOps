// Lighting.hpp
#pragma once

#include <vector>
#include <functional>
#include <cmath>  
#include <algorithm>

#include <glm/vec3.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "../voxels/Chunk.hpp"



static constexpr int PAD = 1;


using BlockGetter = std::function<BlockID(const glm::ivec3&)>;

class Lighting {
public:
    Lighting(int chunkSize = CHUNK_SIZE);





    void prepareChunkAO(
        const glm::ivec3& chunkPos,
        BlockGetter getBlock,
        std::vector<uint8_t>& aoBuffer
    ) const;


    void prepareChunkSunlight(
        const Chunk& ,
        const glm::ivec3& chunkPos,
        BlockGetter getBlock,
        std::vector<float>& sunlightBuffer,
        float sunFalloff // how quickly light dims below occluders
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
};