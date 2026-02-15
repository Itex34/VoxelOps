#include "ChunkMeshBuilder.hpp"

#include "Lighting.hpp"
#include "../voxels/Voxel.hpp"







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

    std::vector<VoxelVertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(4096);
    indices.reserve(6144);

    unsigned short indexOffset = 0;

    Lighting lighting(CHUNK_SIZE);
    std::vector<uint8_t> cornerSun;
    std::vector<uint8_t> cornerAO;

    if (enableShadows) {
        lighting.prepareChunkSunlight(center, chunkPos, neighbors, cornerSun, 1.0f);
    }
    if (enableAO) {
        lighting.prepareChunkAO(center, chunkPos, neighbors, cornerAO);
    }

    const float chunkSizeF = float(CHUNK_SIZE);


    std::vector<GreedyCell> mask(CHUNK_SIZE * CHUNK_SIZE);

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

        // sweep planes
        for (int s = 0; s <= CHUNK_SIZE; ++s) {

            // clear mask
            for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; ++i)
                mask[i] = GreedyCell{};

            // build mask
            for (int j = 0; j < CHUNK_SIZE; ++j) {
                for (int i = 0; i < CHUNK_SIZE; ++i) {

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

                    if ((a != BlockID::Air) == (b != BlockID::Air))
                        continue;

                    GreedyCell& c = mask[j * CHUNK_SIZE + i];
                    c.valid = true;


                    c.sign = (a != BlockID::Air) ? +1 : -1;
                    c.block = (a != BlockID::Air) ? a : b;

                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);

                    auto tex = getTexCoordsForFace(c.block, face, atlas);

                    float tlX = tex[0].x;
                    float tlY = tex[0].y;

                    float brX = tex[2].x;
                    float brY = tex[2].y;


                    float tileW = brX - tlX;
                    float tileH = brY - tlY;

                    int gridX = int(std::round(1.0f / tileW));
                    int tx = int(std::round(tlX / tileW));
                    int ty = int(std::round(tlY / tileH));

                    c.matId = uint8_t(ty * gridX + tx);





                    // AO / Sun
                    if (enableAO || enableShadows) {

                        int sx = i * dux + j * dvx + (c.sign > 0 ? (s - 1) : s) * dx;
                        int sy = i * duy + j * dvy + (c.sign > 0 ? (s - 1) : s) * dy;
                        int sz = i * duz + j * dvz + (c.sign > 0 ? (s - 1) : s) * dz;



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
                                c.sun[k] = cornerSun[ci];
                        }

                        uint32_t key = 0;

                        if (enableAO)
                            key |= (c.ao[0] << 0) |
                            (c.ao[1] << 4) |
                            (c.ao[2] << 8) |
                            (c.ao[3] << 12);

                        if (enableShadows)
                            key |= (c.sun[0] << 16) |
                            (c.sun[1] << 20) |
                            (c.sun[2] << 24) |
                            (c.sun[3] << 28);

                        c.lightKey = key;

                    }
                }
            }


            for (int j = 0; j < CHUNK_SIZE; ++j) {
                for (int i = 0; i < CHUNK_SIZE; ) {

                    GreedyCell& c = mask[j * CHUNK_SIZE + i];
                    if (!c.valid) { ++i; continue; }

                    int w = 1;
                    while (i + w < CHUNK_SIZE) {
                        GreedyCell& r = mask[j * CHUNK_SIZE + (i + w)];

                        if (!r.valid || r.sign != c.sign ||
                            r.block != c.block || r.matId != c.matId ||
                            r.lightKey != c.lightKey) {
                            break;
                        }
                        ++w;
                    }

                    int h = 1;
                    bool stop = false;
                    while (j + h < CHUNK_SIZE && !stop) {
                        for (int k = 0; k < w; ++k) {
                            GreedyCell& r = mask[(j + h) * CHUNK_SIZE + (i + k)];
                            if (!r.valid || r.sign != c.sign ||
                                r.block != c.block || r.matId != c.matId ||
                                r.lightKey != c.lightKey) {
                                stop = true;
                                break;
                            }
                        }
                        if(!stop) ++h;
                    }

                    float ox = float(i * dux + j * dvx + s * dx);
                    float oy = float(i * duy + j * dvy + s * dy);
                    float oz = float(i * duz + j * dvz + s * dz);


                    glm::vec3 vtx[4] = {
                        {ox, oy, oz},
                        {ox + dux * float(w), oy + duy * float(w), oz + duz * float(w)},
                        {ox + dux * float(w) + dvx * float(h), oy + duy * float(w) + dvy * float(h), oz + duz * float(w) + dvz * float(h)},
                        {ox + dvx * float(h), oy + dvy * float(h), oz + dvz * float(h)}
                    };

                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);

                    uint8_t quadAO[4] = { 0,0,0,0 };
                    uint8_t quadSun[4] = { 0,0,0,0 };

                    if (enableAO || enableShadows)
                    {
                        int sx = int(ox);
                        int sy = int(oy);
                        int sz = int(oz);

                        if (c.sign > 0) {
                            sx -= dx;
                            sy -= dy;
                            sz -= dz;
                        }

                        // corners of FINAL GREEDY QUAD
                        int cx[4] = {
                            sx,
                            sx + dux * w,
                            sx + dux * w + dvx * h,
                            sx + dvx * h
                        };

                        int cy[4] = {
                            sy,
                            sy + duy * w,
                            sy + duy * w + dvy * h,
                            sy + dvy * h
                        };

                        int cz[4] = {
                            sz,
                            sz + duz * w,
                            sz + duz * w + dvz * h,
                            sz + dvz * h
                        };

                        for (int k = 0; k < 4; ++k)
                        {
                            int ci = lighting.cornerIndexPadded(cx[k], cy[k], cz[k]);

                            if (enableAO)      quadAO[k] = cornerAO[ci];
                            if (enableShadows) quadSun[k] = cornerSun[ci];
                        }
                    }
                    

                    for (int k = 0; k < 4; ++k) {
                        uint8_t uvCorner = uvRemap[face][k];

                        vertices.push_back(packVoxelVertex(
                            vtx[k],
                            face,
                            uvCorner,
                            c.matId,
                            quadAO[k],
                            quadSun[k]
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
                            mask[(j + yy) * CHUNK_SIZE + (i + xx)].valid = false;

                    i += w;
                }
            }
        }
    }

    return { std::move(vertices), std::move(indices) };
}



