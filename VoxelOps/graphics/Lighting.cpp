#include "Lighting.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "../voxels/Voxel.hpp"

Lighting::Lighting(int chunkSize_)
    : chunkSize(chunkSize_)
    , chunkSizePlus1(chunkSize_ + 1)
    , paddedSize(chunkSize_ + 3) // PAD = 1
{}

void Lighting::buildSolidPadded(
    const Chunk &chunk, const Chunk *neighbors[6], uint8_t *solidPadded
) {
    for (int z = -2; z <= chunkSize + 1; ++z) {
        for (int y = -2; y <= chunkSize + 1; ++y) {
            const int zy = kSolidSize * ((y + kSolidPad) + kSolidSize * (z + kSolidPad));
            for (int x = -2; x <= chunkSize + 1; ++x) {
                solidPadded[(x + kSolidPad) + zy] =
                    uint8_t(isSolidSafePadded(x, y, z, chunk, neighbors) ? 1 : 0);
            }
        }
    }
}

void Lighting::prepareChunkAO(
    const Chunk &chunk,
    const glm::ivec3 &chunkPos,
    const Chunk *neighbors[6],
    uint8_t *aoBuffer,
    const uint8_t *solidPadded
) {
    (void)chunkPos;

    const int paddedStrideZ = kPaddedSize * kPaddedSize;

    //fully lit
    std::fill_n(aoBuffer, kPaddedVolume, uint8_t(15));

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
                //neighbor occupancy along each axis
                const uint8_t solidNegX = solidPadded[solidIndex(x - 1, y, z)];

                const uint8_t solidNegY = solidPadded[solidIndex(x, y - 1, z)];

                const uint8_t solidNegZ = solidPadded[solidIndex(x, y, z - 1)];

                //corner occupancy where two axis-neighbors meet
                const uint8_t solidNegXNegY =
                    uint8_t(
                        solidNegX & 
                        solidNegY & 
                        solidPadded[solidIndex(x - 1, y - 1, z)]
                    );

                const uint8_t solidNegXNegZ =
                    uint8_t(
                        solidNegX & 
                        solidNegZ & 
                        solidPadded[solidIndex(x - 1, y, z - 1)]
                    );

                const uint8_t solidNegYNegZ =
                    uint8_t(
                        solidNegY & 
                        solidNegZ & 
                        solidPadded[solidIndex(x, y - 1, z - 1)]
                    );

                const int occlusion = 
                    int(solidNegX) 
                    + int(solidNegY) 
                    + int(solidNegZ) 
                    + int(solidNegXNegY) 
                    + int(solidNegXNegZ) 
                    + int(solidNegYNegZ);

                const uint8_t ao = uint8_t(15 - occlusion * 2);

                aoBuffer[outZY + (x + PAD)] = ao;
            }
        }
    }
}

// lighting helper: compute corner indices for a face in the exact order that the mesh emits
// vtx[0..3].
void Lighting::faceCornerIndicesForCell(
    int sx,
    int sy,
    int sz,       // sampling cell base (see notes below)
    int face,     // 0..5 (same enum as your mesher)
    int outIdx[4] // returns 4 corner indices in BL, BR, TR, TL order
) const {
    auto idx = [&](int X, int Y, int Z) { return cornerIndexPadded(X, Y, Z); };

    switch (face) {
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
