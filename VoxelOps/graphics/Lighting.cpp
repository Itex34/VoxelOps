#include "Lighting.hpp"
#include <algorithm>
#include <cmath>

#include "../voxels/Voxel.hpp"


Lighting::Lighting(int chunkSize_)
    : chunkSize(chunkSize_),
    paddedSize(chunkSize_ + 3) // PAD = 1
{
}









void Lighting::prepareChunkSunlight(
    const Chunk& chunk,
    const glm::ivec3& chunkPos,
    const Chunk* neighbors[6],
    std::vector<uint8_t>& sunlightBuffer,
    float sunFalloff // how quickly light dims below occluders
) const
{
    const int total = paddedSize * paddedSize * paddedSize;
    sunlightBuffer.assign(total, 1.0f);

    glm::ivec3 base = chunkPos * chunkSize;

    for (int z = -1; z <= chunkSize + 1; ++z) {
        for (int x = -1; x <= chunkSize + 1; ++x) {
            float light = 1.0f;

            // Check sky access for this column
            for (int y = chunkSize + 2; y < chunkSize + 64; ++y) {
                int wx = x;
                int wz = z;

                if (isSolidSafe(x, y, z, chunk, neighbors)) {
                    light = 0.0f;
                    break;
                }


            }

            // Propagate downward with deterministic accumulation
            for (int y = chunkSize + 1; y >= -1; --y) {
                // Store light for THIS corner
                float cornerLight = light;

                // Count blockers at this level (affecting corners BELOW)
                int blockers = 0;
                for (int ox = 0; ox <= 1; ++ox) {
                    for (int oz = 0; oz <= 1; ++oz) {
                        int bx = x - (1 - ox);
                        int bz = z - (1 - oz);

                        if (isSolidSafe(bx, y, bz, chunk, neighbors))
                            ++blockers;

                    }
                }

                // Update light for NEXT iteration (lower Y)
                if (blockers > 0) {
                    if (sunFalloff >= 1.0f) {
                        light = 0.0f;
                    }
                    else {
                        // Deterministic: reduce by fixed amount per blocker
                        int lightLevel = int(light * 15.0f + 0.5f);
                        lightLevel = std::max(0, lightLevel - blockers * 2); // 2/15 per blocker
                        light = float(lightLevel) / 15.0f;
                    }
                }

                uint8_t level = uint8_t(std::round(cornerLight * 15.0f));
                sunlightBuffer[cornerIndexPadded(x, y, z)] = level;
            }
        }
    }
}











void Lighting::prepareChunkAO(
    const Chunk& chunk,
    const glm::ivec3& chunkPos,
    const Chunk* neighbors[6],
    std::vector<uint8_t>& aoBuffer
) const
{
    const int total = paddedSize * paddedSize * paddedSize;
    aoBuffer.assign(total, 15); // Default to full brightness
    
    glm::ivec3 base = chunkPos * chunkSize;
    
    // For each corner position in padded space
    for (int z = -1; z <= chunkSize + 1; ++z) {
        for (int y = -1; y <= chunkSize + 1; ++y) {
            for (int x = -1; x <= chunkSize + 1; ++x) {
                glm::ivec3 local(x, y, z);

                bool sx = isSolidSafe(x - 1, y, z, chunk, neighbors);
                bool sy = isSolidSafe(x, y - 1, z, chunk, neighbors);
                bool sz = isSolidSafe(x, y, z - 1, chunk, neighbors);

                // Diagonal only if both sides exist
                bool sxy = (sx && sy) && isSolidSafe(x - 1, y - 1, z, chunk, neighbors);
                bool sxz = (sx && sz) && isSolidSafe(x - 1, y, z - 1, chunk, neighbors);
                bool syz = (sy && sz) && isSolidSafe(x, y - 1, z - 1, chunk, neighbors);

                int occlusion = 0;
                occlusion += sx;
                occlusion += sy;
                occlusion += sz;
                occlusion += sxy;
                occlusion += sxz;
                occlusion += syz;

                uint8_t ao = uint8_t(std::max(0, 15 - occlusion * 2));
                aoBuffer[cornerIndexPadded(x, y, z)] = ao;

            }
        }
    }
}
