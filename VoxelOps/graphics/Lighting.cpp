#include "Lighting.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "../voxels/Voxel.hpp"


Lighting::Lighting(int chunkSize_)
    : chunkSize(chunkSize_),
    chunkSizePlus1(chunkSize_ + 1),
    paddedSize(chunkSize_ + 3) // PAD = 1
{
}


void Lighting::prepareChunkSunlight(
    const Chunk& chunk,
    const glm::ivec3& chunkPos,
    const Chunk* neighbors[6],
    uint8_t* sunlightBuffer,
    float sunFalloff,
    const TopOccluderGetter& getTopOccluderY,
    const uint8_t* solidPadded
)
{
    (void)sunFalloff;
    std::fill_n(sunlightBuffer, kPaddedVolume, uint8_t(0));

    auto solidIndex = [](int x, int y, int z) -> int {
        return (x + kSolidPad) + kSolidSize * ((y + kSolidPad) + kSolidSize * (z + kSolidPad));
    };

    thread_local std::array<uint8_t, kSolidVolume> localSolid{};
    if (!solidPadded && !getTopOccluderY) {
        buildSolidPadded(chunk, neighbors, localSolid.data());
        solidPadded = localSolid.data();
    }

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
                const bool blocked =
                    (solidPadded[solidIndex(x - 1, y, z - 1)] != 0) |
                    (solidPadded[solidIndex(x, y, z - 1)] != 0) |
                    (solidPadded[solidIndex(x - 1, y, z)] != 0) |
                    (solidPadded[solidIndex(x, y, z)] != 0);

                if (blocked) {
                    light = (light > 2) ? uint8_t(light - 2) : uint8_t(0);
                }
                sunlightBuffer[cornerIndexPadded(x, y, z)] = light;
            }
        }
    }
}


void Lighting::buildSolidPadded(
    const Chunk& chunk,
    const Chunk* neighbors[6],
    uint8_t* solidPadded
)
{
    for (int z = -2; z <= chunkSize + 1; ++z) {
        for (int y = -2; y <= chunkSize + 1; ++y) {
            const int zy = kSolidSize * ((y + kSolidPad) + kSolidSize * (z + kSolidPad));
            for (int x = -2; x <= chunkSize + 1; ++x) {
                solidPadded[(x + kSolidPad) + zy] = uint8_t(isSolidSafePadded(x, y, z, chunk, neighbors) ? 1 : 0);
            }
        }
    }
}


void Lighting::prepareChunkAO(
    const Chunk& chunk,
    const glm::ivec3& chunkPos,
    const Chunk* neighbors[6],
    uint8_t* aoBuffer,
    const uint8_t* solidPadded
)
{
    (void)chunkPos;
    const int paddedStrideZ = kPaddedSize * kPaddedSize;
    std::fill_n(aoBuffer, kPaddedVolume, uint8_t(15)); // full light

    auto solidIndex = [](int x, int y, int z) -> int {
        return (x + kSolidPad) + kSolidSize * ((y + kSolidPad) + kSolidSize * (z + kSolidPad));
    };

    thread_local std::array<uint8_t, kSolidVolume> localSolid{};
    if (!solidPadded) {
        buildSolidPadded(chunk, neighbors, localSolid.data());
        solidPadded = localSolid.data();
    }

    for (int z = -1; z <= chunkSize + 1; ++z) {
        const int outZ = (z + PAD) * paddedStrideZ;
        for (int y = -1; y <= chunkSize + 1; ++y) {
            const int outZY = outZ + (y + PAD) * kPaddedSize;
            for (int x = -1; x <= chunkSize + 1; ++x) {
                const int i_sx = solidIndex(x - 1, y, z);
                const int i_sy = solidIndex(x, y - 1, z);
                const int i_sz = solidIndex(x, y, z - 1);
                const uint8_t sx = solidPadded[i_sx];
                const uint8_t sy = solidPadded[i_sy];
                const uint8_t sz = solidPadded[i_sz];

                const uint8_t sxy = uint8_t(sx & sy & solidPadded[solidIndex(x - 1, y - 1, z)]);
                const uint8_t sxz = uint8_t(sx & sz & solidPadded[solidIndex(x - 1, y, z - 1)]);
                const uint8_t syz = uint8_t(sy & sz & solidPadded[solidIndex(x, y - 1, z - 1)]);

                const int occlusion = int(sx) + int(sy) + int(sz) + int(sxy) + int(sxz) + int(syz);
                const uint8_t ao = uint8_t(15 - occlusion * 2);
                aoBuffer[outZY + (x + PAD)] = ao;
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
