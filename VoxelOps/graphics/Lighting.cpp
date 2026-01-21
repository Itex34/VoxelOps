#include "Lighting.hpp"
#include <algorithm>
#include <cmath>


#include "../voxels/Voxel.hpp"


Lighting::Lighting(int chunkSize_)
    : chunkSize(chunkSize_),
    paddedSize(chunkSize_ + 3) // PAD = 1
{
}






// ------------------------------------------------------------
// Compute AO for a single corner
// ------------------------------------------------------------
float Lighting::computeCornerAO(
    const glm::ivec3& solidVoxelWorld,
    int face,
    int corner,
    BlockGetter getBlock
) const
{
    // Empty voxel in front of the face
    glm::ivec3 emptyVoxel = solidVoxelWorld + faceNormals[face];

    const glm::ivec3& U = faceAxisU[face];
    const glm::ivec3& V = faceAxisV[face];

    int su = FACE_CORNER_SIGNS[face][corner][0];
    int sv = FACE_CORNER_SIGNS[face][corner][1];

    // Sample neighboring VOXELS (not corners)
    glm::ivec3 sideU = emptyVoxel + su * U;
    glm::ivec3 sideV = emptyVoxel + sv * V;
    glm::ivec3 diag = emptyVoxel + su * U + sv * V;

    bool occU = getBlock(sideU) != BlockID::Air;
    bool occV = getBlock(sideV) != BlockID::Air;
    bool occD = getBlock(diag) != BlockID::Air;


    int occ = int(occU) + int(occV) + int(occD);

    static constexpr float AO_TABLE[4] = {
        1.0f, 0.9f, 0.8f, 0.7f
    };

    return AO_TABLE[occ];
}




// ------------------------------------------------------------
// Prepare padded chunk sunlight
// ------------------------------------------------------------
void Lighting::prepareChunkSunlight(
    const Chunk& /*chunk*/,
    const glm::ivec3& chunkPos,
    BlockGetter getBlock,
    std::vector<float>& sunlightBuffer,
    float sunFalloff
) const
{
    const int total = paddedSize * paddedSize * paddedSize;
    sunlightBuffer.assign(total, 1.0f);

    glm::ivec3 base = chunkPos * chunkSize;

    for (int z = -1; z <= chunkSize + 1; ++z) {
        for (int x = -1; x <= chunkSize + 1; ++x) {

            float light = 1.0f;

            // Find first blocking voxel above this chunk
            for (int y = chunkSize + 2; y < chunkSize + 64; ++y) {
                glm::ivec3 w = base + glm::ivec3(x, y, z);
                if (getBlock(w) != BlockID::Air) {
                    light = 0.0f;
                    break;
                }
            }

            // Now propagate downward consistently
            for (int y = chunkSize + 1; y >= -1; --y) {

                int blockers = 0;

                for (int ox = 0; ox <= 1; ++ox) {
                    for (int oz = 0; oz <= 1; ++oz) {

                        glm::ivec3 voxelWorld =
                            base + glm::ivec3(x - (1 - ox), y, z - (1 - oz));

                        if (getBlock(voxelWorld) != BlockID::Air)
                            ++blockers;
                    }
                }

                if (blockers > 0) {
                    if (sunFalloff >= 1.0f)
                        light = 0.0f;
                    else
                        light *= std::pow(sunFalloff, float(blockers));
                }

                sunlightBuffer[cornerIndexPadded(x, y, z)] = light;
            }
        }
    }
}
