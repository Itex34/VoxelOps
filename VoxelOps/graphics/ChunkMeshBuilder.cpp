#include "ChunkMeshBuilder.hpp"

#include "Lighting.hpp"
#include "../voxels/Voxel.hpp"



// returns 1 → sun visible, 0 → fully occluded
[[nodiscard]] static float sunAtCorner(
    const std::function<BlockID(const glm::ivec3&)>& getBlockWorld,
    const glm::ivec3& cornerWorld,   // integer world position of the corner
    float sunFalloff)                // 1.0 = hard shadow, <1 softer
{
    constexpr int WORLD_TOP_Y = 320;          // your build height
    constexpr int WORLD_BOTTOM_Y = -64;       // your min height

    int hit = 0;
    for (int y = cornerWorld.y + 1; y <= WORLD_TOP_Y; ++y)   // march upward
    {
        if (getBlockWorld({ cornerWorld.x, y, cornerWorld.z }) != BlockID::Air)
        {
            ++hit;
            if (sunFalloff >= 1.0f) return 0.0f;   // first solid → full shadow
        }
    }
    if (hit == 0) return 1.0f;                     // reached open sky
    return std::pow(sunFalloff, static_cast<float>(hit));
}







inline uint32_t quantizePos5(float v) {
    // assumes v in [0, 16], which your code guarantees
    int iv = int(v * (31.0f / 16.0f) + 0.00001f);
    if (iv < 0) iv = 0;
    if (iv > 31) iv = 31;
    return uint32_t(iv);
}









inline uint32_t clampToCorner(float v) {
    uint32_t iv = uint32_t(v);
#ifndef NDEBUG
    assert(iv <= 16);
#endif
    return iv & 0x1Fu;
}



inline VoxelVertex packVoxelVertex(
    const glm::vec3& posLocal,
    uint8_t face,
    uint8_t corner,
    uint8_t matId,
    uint8_t ao,     // 0..15
    uint8_t sun     // 0..15
) {
    uint32_t qx = uint32_t(posLocal.x) & 0x1Fu;
    uint32_t qy = uint32_t(posLocal.y) & 0x1Fu;
    uint32_t qz = uint32_t(posLocal.z) & 0x1Fu;

    uint32_t low =
        qx
        | (qy << 5)
        | (qz << 10)
        | ((face & 0x7u) << 15)
        | ((corner & 0x3u) << 18)
        | ((ao & 0xFu) << 26);

    uint32_t high =
        (uint32_t(matId) << 0)
        | (uint32_t(sun & 0xFu) << 8);

    return { low, high };
}












BuiltChunkMesh ChunkMeshBuilder::buildChunkMesh(
    const Chunk& center,
    const Chunk* neighbors[6],
    const glm::ivec3& chunkPos,
    const TextureAtlas& atlas,
    bool enableAO,
    bool enableShadows
)
{
    constexpr int CS = CHUNK_SIZE; // 16

    std::vector<VoxelVertex> vertices;
    std::vector<unsigned short> indices;
    vertices.reserve(4096);
    indices.reserve(6144);

    unsigned short indexOffset = 0;

    // ------------------------------------------------------------
    // Lighting
    // ------------------------------------------------------------
    Lighting lighting(CS);
    std::vector<float> cornerSun;
    std::vector<uint8_t> cornerAO;

    if (enableShadows) {
        lighting.prepareChunkSunlight(center, chunkPos, neighbors, cornerSun, 1.0f);
    }
    if (enableAO) {
        lighting.prepareChunkAO(center, chunkPos, neighbors, cornerAO);
    }

    const float chunkSizeF = float(CS);

    // ------------------------------------------------------------
    // Greedy mask
    // ------------------------------------------------------------
    std::vector<GreedyCell> mask(CS * CS);

    // ------------------------------------------------------------
    // Axis sweeps
    // ------------------------------------------------------------
    for (int d = 0; d < 3; ++d) {

        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        // Axis basis vectors (flattened)
        int dux = 0, duy = 0, duz = 0;
        int dvx = 0, dvy = 0, dvz = 0;
        int dx = 0, dy = 0, dz = 0;

        switch (d) {
        case 0: dx = 1; break;
        case 1: dy = 1; break;
        case 2: dz = 1; break;
        }

        switch (u) {
        case 0: dux = 1; break;
        case 1: duy = 1; break;
        case 2: duz = 1; break;
        }

        switch (v) {
        case 0: dvx = 1; break;
        case 1: dvy = 1; break;
        case 2: dvz = 1; break;
        }

        // Sweep planes
        for (int s = 0; s <= CS; ++s) {

            // Clear mask
            for (int i = 0; i < CS * CS; ++i)
                mask[i] = GreedyCell{};


            // --------------------------------------------------
            // Build mask
            // --------------------------------------------------
            for (int j = 0; j < CS; ++j) {
                for (int i = 0; i < CS; ++i) {

                    // pa = (i, j, s-1) projected on axes
                    int pax = i * dux + j * dvx + (s - 1) * dx;
                    int pay = i * duy + j * dvy + (s - 1) * dy;
                    int paz = i * duz + j * dvz + (s - 1) * dz;

                    // pb = (i, j, s)
                    int pbx = i * dux + j * dvx + s * dx;
                    int pby = i * duy + j * dvy + s * dy;
                    int pbz = i * duz + j * dvz + s * dz;

                    BlockID a = getBlockSafe(pax, pay, paz, center, neighbors);
                    BlockID b = getBlockSafe(pbx, pby, pbz, center, neighbors);

                    // Same solidity → no face
                    if ((a != BlockID::Air) == (b != BlockID::Air))
                        continue;

                    GreedyCell& c = mask[j * CS + i];
                    c.valid = true;
                    c.sign = (a != BlockID::Air) ? +1 : -1;
                    c.block = (a != BlockID::Air) ? a : b;

                    // Face index (identical logic)
                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);

                    // Material ID (unchanged behavior)
                    auto tex = getTexCoordsForFace(c.block, face, atlas);
                    glm::vec2 tl = tex[0];
                    glm::vec2 br = tex[2];

                    float tileW = br.x - tl.x;
                    float tileH = br.y - tl.y;
                    int gridX = int(std::round(1.0f / tileW));
                    int tx = int(std::round(tl.x / tileW));
                    int ty = int(std::round(tl.y / tileH));
                    c.matId = uint8_t(ty * gridX + tx);

                    // AO / Sun
                    if (enableAO || enableShadows) {

                        int sx = (c.sign > 0) ? pax + dx : pbx;
                        int sy = (c.sign > 0) ? pay + dy : pby;
                        int sz = (c.sign > 0) ? paz + dz : pbz;



                        int cx0 = sx;
                        int cy0 = sy;
                        int cz0 = sz;

                        int cx1 = sx + dux;
                        int cy1 = sy + duy;
                        int cz1 = sz + duz;

                        int cx2 = cx1 + dvx;
                        int cy2 = cy1 + dvy;
                        int cz2 = cz1 + dvz;

                        int cx3 = sx + dvx;
                        int cy3 = sy + dvy;
                        int cz3 = sz + dvz;

                        int cix[4] = { cx0, cx1, cx2, cx3 };
                        int ciy[4] = { cy0, cy1, cy2, cy3 };
                        int ciz[4] = { cz0, cz1, cz2, cz3 };

                        for (int k = 0; k < 4; ++k) {
                            int ci = lighting.cornerIndexPadded(cix[k], ciy[k], ciz[k]);
                            if (enableAO)
                                c.ao[k] = cornerAO[ci];
                            if (enableShadows)
                                c.sun[k] = uint8_t(cornerSun[ci] * 15.f + 0.5f);
                        }

                    }
                }
            }

            // --------------------------------------------------
            // Greedy merge + emit
            // --------------------------------------------------
            for (int j = 0; j < CS; ++j) {
                for (int i = 0; i < CS; ) {

                    GreedyCell& c = mask[j * CS + i];
                    if (!c.valid) { ++i; continue; }

                    int w = 1;
                    while (i + w < CS) {
                        GreedyCell& r = mask[j * CS + (i + w)];
                        if (!r.valid || r.sign != c.sign ||
                            r.block != c.block || r.matId != c.matId ||
                            r.ao[0] != c.ao[0] || r.ao[1] != c.ao[1] ||
                            r.ao[2] != c.ao[2] || r.ao[3] != c.ao[3] ||
                            r.sun[0] != c.sun[0] || r.sun[1] != c.sun[1] ||
                            r.sun[2] != c.sun[2] || r.sun[3] != c.sun[3])
                            break;
                        ++w;
                    }

                    int h = 1;
                    bool stop = false;
                    while (j + h < CS && !stop) {
                        for (int k = 0; k < w; ++k) {
                            GreedyCell& r = mask[(j + h) * CS + (i + k)];
                            if (!r.valid || r.sign != c.sign ||
                                r.block != c.block || r.matId != c.matId ||
                                r.ao[0] != c.ao[0] || r.ao[1] != c.ao[1] ||
                                r.ao[2] != c.ao[2] || r.ao[3] != c.ao[3] ||
                                r.sun[0] != c.sun[0] || r.sun[1] != c.sun[1] ||
                                r.sun[2] != c.sun[2] || r.sun[3] != c.sun[3]) {
                                stop = true;
                                break;
                            }
                        }
                        if (!stop) ++h;
                    }

                    // Emit quad (same math, flattened)
                    float ox = float(i * dux + j * dvx + s * dx);
                    float oy = float(i * duy + j * dvy + s * dy);
                    float oz = float(i * duz + j * dvz + s * dz);

                    glm::vec3 du = glm::vec3(dux, duy, duz) * float(w);
                    glm::vec3 dv = glm::vec3(dvx, dvy, dvz) * float(h);

                    glm::vec3 vtx[4] = {
                        {ox, oy, oz},
                        {ox + du.x, oy + du.y, oz + du.z},
                        {ox + du.x + dv.x, oy + du.y + dv.y, oz + du.z + dv.z},
                        {ox + dv.x, oy + dv.y, oz + dv.z}
                    };

                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);

                    for (int k = 0; k < 4; ++k) {
                        uint8_t uvCorner = uvRemap[face][k];
                        vertices.push_back(packVoxelVertex(
                            vtx[k],
                            face,
                            uvCorner,
                            c.matId,
                            c.ao[k],
                            c.sun[k]
                        ));
                    }

                    if (c.sign > 0) {
                        indices.push_back(indexOffset + 0);
                        indices.push_back(indexOffset + 1);
                        indices.push_back(indexOffset + 2);
                        indices.push_back(indexOffset + 0);
                        indices.push_back(indexOffset + 2);
                        indices.push_back(indexOffset + 3);
                    }
                    else {
                        indices.push_back(indexOffset + 0);
                        indices.push_back(indexOffset + 2);
                        indices.push_back(indexOffset + 1);
                        indices.push_back(indexOffset + 0);
                        indices.push_back(indexOffset + 3);
                        indices.push_back(indexOffset + 2);
                    }

                    indexOffset += 4;

                    for (int yy = 0; yy < h; ++yy)
                        for (int xx = 0; xx < w; ++xx)
                            mask[(j + yy) * CS + (i + xx)].valid = false;

                    i += w;
                }
            }
        }
    }

    return { std::move(vertices), std::move(indices) };
}



