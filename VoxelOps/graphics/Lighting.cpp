#include "Lighting.hpp"
#include <algorithm>
#include <cmath>


#include "../voxels/Voxel.hpp"


Lighting::Lighting(int chunkSize_)
    : chunkSize(chunkSize_),
    paddedSize(chunkSize_ + 3) // PAD = 1
{
}


// In Lighting.hpp
static constexpr float SUN_FALLOFF_TABLE[5] = {
    1.0f,       // 0 blockers
    0.85f,      // 1 blocker
    0.7225f,    // 2 blockers
    0.614125f,  // 3 blockers
    0.52200625f // 4 blockers
};






 

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

            // Check sky access for this column
            for (int y = chunkSize + 2; y < chunkSize + 64; ++y) {
                glm::ivec3 w = base + glm::ivec3(x, y, z);
                if (getBlock(w) != BlockID::Air) {
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
                        glm::ivec3 voxelWorld = base + glm::ivec3(x - (1 - ox), y, z - (1 - oz));
                        if (getBlock(voxelWorld) != BlockID::Air) ++blockers;
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

                // Quantize for cross-chunk consistency
                sunlightBuffer[cornerIndexPadded(x, y, z)] = std::round(cornerLight * 15.0f) / 15.0f;
            }
        }
    }
}













void Lighting::prepareChunkAO(
    const glm::ivec3& chunkPos,
    BlockGetter getBlock,
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
                glm::ivec3 cornerWorld = base + glm::ivec3(x, y, z);
                
                // Count solid voxels touching this corner (8 possible)
                int solidCount = 0;
                for (int dz = -1; dz <= 0; ++dz) {
                    for (int dy = -1; dy <= 0; ++dy) {
                        for (int dx = -1; dx <= 0; ++dx) {
                            if (getBlock(cornerWorld + glm::ivec3(dx, dy, dz)) != BlockID::Air) {
                                ++solidCount;
                            }
                        }
                    }
                }
                
                // Map 0-8 solid neighbors to AO value (0-15 range)
                // 0 solids = full light (15), 8 solids = darkest (0)
                uint8_t ao = uint8_t(std::max(0, 15 - solidCount * 2));
                aoBuffer[cornerIndexPadded(x, y, z)] = ao;
            }
        }
    }
}
