#include "Lighting.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

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
    float sunFalloff,
    const TopOccluderGetter& getTopOccluderY
)
{
    int paddedSize = chunkSize + 3;
    sunlightBuffer.assign(paddedSize * paddedSize * paddedSize, 0);

    if (getTopOccluderY) {
        const int chunkWorldMinX = chunkPos.x * CHUNK_SIZE;
        const int chunkWorldMinY = chunkPos.y * CHUNK_SIZE;
        const int chunkWorldMinZ = chunkPos.z * CHUNK_SIZE;

        for (int z = -1; z <= chunkSize + 1; ++z) {
            for (int x = -1; x <= chunkSize + 1; ++x) {
                const int worldX = chunkWorldMinX + x;
                const int worldZ = chunkWorldMinZ + z;

                int topOccluderY = std::numeric_limits<int>::min();
                for (int ox = 0; ox <= 1; ++ox) {
                    for (int oz = 0; oz <= 1; ++oz) {
                        topOccluderY = std::max(topOccluderY, getTopOccluderY(worldX + ox - 1, worldZ + oz - 1));
                    }
                }

                for (int y = chunkSize + 1; y >= -1; --y) {
                    const int worldY = chunkWorldMinY + y;
                    const int blockedLayers = (worldY <= topOccluderY) ? (topOccluderY - worldY + 1) : 0;
                    const int light = std::max(0, 15 - blockedLayers * 2);
                    sunlightBuffer[cornerIndexPadded(x, y, z)] = uint8_t(light);
                }
            }
        }
        return;
    }

    for (int z = -1; z <= chunkSize + 1; ++z) {
        for (int x = -1; x <= chunkSize + 1; ++x) {
            uint8_t light = 15;
            for (int y = chunkSize + 1; y >= -1; --y) {
                bool blocked = false;
                for (int ox = 0; ox <= 1; ++ox)
                    for (int oz = 0; oz <= 1; ++oz)
                        blocked |= isSolidSafePadded(x + ox - 1, y, z + oz - 1, chunk, neighbors);

                if (blocked) {
                    light = (light > 2) ? uint8_t(light - 2) : uint8_t(0);
                }
                sunlightBuffer[cornerIndexPadded(x, y, z)] = light;
            }
        }
    }
}










void Lighting::prepareChunkAO(
    const Chunk& chunk,
    const glm::ivec3& chunkPos,
    const Chunk* neighbors[6],
    std::vector<uint8_t>& aoBuffer
) 
{
    int paddedSize = chunkSize + 3;
    aoBuffer.assign(paddedSize * paddedSize * paddedSize, 15); // full light

    for (int z = -1; z <= chunkSize + 1; ++z) {
        for (int y = -1; y <= chunkSize + 1; ++y) {
            for (int x = -1; x <= chunkSize + 1; ++x) {

                int occlusion = 0;

                bool sx = isSolidSafePadded(x - 1, y, z, chunk, neighbors);
                bool sy = isSolidSafePadded(x, y - 1, z, chunk, neighbors);
                bool sz = isSolidSafePadded(x, y, z - 1, chunk, neighbors);

                bool sxy = sx && sy && isSolidSafePadded(x - 1, y - 1, z, chunk, neighbors);
                bool sxz = sx && sz && isSolidSafePadded(x - 1, y, z - 1, chunk, neighbors);
                bool syz = sy && sz && isSolidSafePadded(x, y - 1, z - 1, chunk, neighbors);

                occlusion = sx + sy + sz + sxy + sxz + syz;

                uint8_t ao = uint8_t(std::max(0, 15 - occlusion * 2));
                aoBuffer[cornerIndexPadded(x, y, z)] = ao;
            }
        }
    }
}

 


// lighting helper: compute corner indices for a face in the exact order that the mesh emits vtx[0..3].
void Lighting::faceCornerIndicesForCell(
    int sx, int sy, int sz,   // sampling cell base (see notes below)
    int face,                 // 0..5 (same enum as your mesher)
    int outIdx[4]             // returns 4 corner indices in BL, BR, TR, TL order
) const
{
    auto idx = [&](int X, int Y, int Z) { return cornerIndexPadded(X, Y, Z); };

    switch (face)
    {
    case 0: // +X
        // BL, BR, TR, TL  (match your vtx layout)
        outIdx[0] = idx(sx + 1, sy, sz);
        outIdx[1] = idx(sx + 1, sy, sz + 1);
        outIdx[2] = idx(sx + 1, sy + 1, sz + 1);
        outIdx[3] = idx(sx + 1, sy + 1, sz);
        break;

    case 1: // -X
        outIdx[0] = idx(sx, sy, sz + 1);
        outIdx[1] = idx(sx, sy, sz);
        outIdx[2] = idx(sx, sy + 1, sz);
        outIdx[3] = idx(sx, sy + 1, sz + 1);
        break;

    case 2: // +Y
        outIdx[0] = idx(sx, sy + 1, sz);
        outIdx[1] = idx(sx + 1, sy + 1, sz);
        outIdx[2] = idx(sx + 1, sy + 1, sz + 1);
        outIdx[3] = idx(sx, sy + 1, sz + 1);
        break;

    case 3: // -Y
        outIdx[0] = idx(sx, sy, sz + 1);
        outIdx[1] = idx(sx + 1, sy, sz + 1);
        outIdx[2] = idx(sx + 1, sy, sz);
        outIdx[3] = idx(sx, sy, sz);
        break;

    case 4: // +Z
        outIdx[0] = idx(sx, sy, sz + 1);
        outIdx[1] = idx(sx + 1, sy, sz + 1);
        outIdx[2] = idx(sx + 1, sy + 1, sz + 1);
        outIdx[3] = idx(sx, sy + 1, sz + 1);
        break;

    case 5: // -Z
        outIdx[0] = idx(sx + 1, sy, sz);
        outIdx[1] = idx(sx, sy, sz);
        outIdx[2] = idx(sx, sy + 1, sz);
        outIdx[3] = idx(sx + 1, sy + 1, sz);
        break;

    default:
        outIdx[0] = outIdx[1] = outIdx[2] = outIdx[3] = 0;
    }
}
