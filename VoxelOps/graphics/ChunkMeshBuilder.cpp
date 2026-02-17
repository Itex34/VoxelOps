#include "ChunkMeshBuilder.hpp"

#include "Lighting.hpp"
#include "../voxels/Voxel.hpp"
#include <atomic>
#include <chrono>
#include <array>
#include <mutex>

namespace {
    using Clock = std::chrono::steady_clock;

    std::atomic<uint64_t> g_profileChunks{ 0 };
    std::atomic<uint64_t> g_profileTotalUs{ 0 };
    std::atomic<uint64_t> g_profileBlockGridUs{ 0 };
    std::atomic<uint64_t> g_profileSolidCacheUs{ 0 };
    std::atomic<uint64_t> g_profileSunlightPrepUs{ 0 };
    std::atomic<uint64_t> g_profileAoPrepUs{ 0 };
    std::atomic<uint64_t> g_profileMaskTransitionUs{ 0 };
    std::atomic<uint64_t> g_profileMaskLightingUs{ 0 };
    std::atomic<uint64_t> g_profileMaskBuildUs{ 0 };
    std::atomic<uint64_t> g_profileGreedyEmitUs{ 0 };

    using MatIdLut = std::array<std::array<uint8_t, 6>, size_t(BlockID::COUNT)>;

    MatIdLut buildMatIdLut(const TextureAtlas& atlas) {
        MatIdLut lut{};
        for (size_t bi = 0; bi < size_t(BlockID::COUNT); ++bi) {
            const BlockID bid = static_cast<BlockID>(bi);
            auto bit = blockTypes.find(bid);
            if (bit == blockTypes.end()) {
                continue;
            }

            const BlockType& bt = bit->second;
            for (int face = 0; face < 6; ++face) {
                const std::string* tileName = nullptr;
                switch (face) {
                case 0:
                case 1:
                    tileName = &bt.textures.RLSide;
                    break;
                case 4:
                case 5:
                    tileName = &bt.textures.FBSide;
                    break;
                case 2:
                    tileName = &bt.textures.top;
                    break;
                case 3:
                    tileName = &bt.textures.bottom;
                    break;
                default:
                    break;
                }

                if (tileName == nullptr || tileName->empty()) {
                    lut[bi][face] = 0;
                    continue;
                }

                auto tit = atlas.tileMap.find(*tileName);
                if (tit == atlas.tileMap.end()) {
                    lut[bi][face] = 0;
                    continue;
                }

                const glm::ivec2 tile = tit->second;
                lut[bi][face] = uint8_t(tile.y * TEXTURE_ATLAS_SIZE + tile.x);
            }
        }
        return lut;
    }

    const MatIdLut& getCachedMatIdLut(const TextureAtlas& atlas) {
        static std::once_flag once;
        static MatIdLut lut{};
        std::call_once(once, [&]() {
            lut = buildMatIdLut(atlas);
        });
        return lut;
    }
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
    bool enableShadows,
    const SunTopGetter& getSunTopY
)
{
    const auto tTotal0 = Clock::now();
    uint64_t blockGridUs = 0;
    uint64_t solidCacheUs = 0;
    uint64_t sunlightPrepUs = 0;
    uint64_t aoPrepUs = 0;
    Clock::duration maskTransitionDur{};
    Clock::duration maskLightingDur{};
    Clock::duration greedyEmitDur{};

    std::vector<VoxelVertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(4096);
    indices.reserve(6144);

    if (center.isCompletelyAir()) {
        const uint64_t totalUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tTotal0).count());
        g_profileChunks.fetch_add(1, std::memory_order_relaxed);
        g_profileTotalUs.fetch_add(totalUs, std::memory_order_relaxed);
        return { std::move(vertices), std::move(indices) };
    }

    unsigned short indexOffset = 0;

    Lighting lighting(CHUNK_SIZE);
    thread_local std::array<uint8_t, Lighting::kPaddedVolume> cornerSun{};
    thread_local std::array<uint8_t, Lighting::kPaddedVolume> cornerAO{};
    thread_local std::array<uint8_t, Lighting::kSolidVolume> solidPadded{};

    if (enableAO || enableShadows) {
        const auto tSolid0 = Clock::now();
        lighting.buildSolidPadded(center, neighbors, solidPadded.data());
        solidCacheUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tSolid0).count());
    }

    constexpr int gridSize = CHUNK_SIZE + 2; // coordinates in [-1 .. CHUNK_SIZE]
    std::array<uint8_t, gridSize * gridSize * gridSize> blockGrid{};
    auto gridIndex = [gridSize](int x, int y, int z) -> int {
        return (x + 1) + gridSize * ((y + 1) + gridSize * (z + 1));
    };
    const auto tGrid0 = Clock::now();
    for (int z = -1; z <= CHUNK_SIZE; ++z) {
        for (int y = -1; y <= CHUNK_SIZE; ++y) {
            for (int x = -1; x <= CHUNK_SIZE; ++x) {
                blockGrid[gridIndex(x, y, z)] = uint8_t(getBlockSafe(x, y, z, center, neighbors));
            }
        }
    }
    blockGridUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tGrid0).count());

    if (enableShadows) {
        const auto t0 = Clock::now();
        lighting.prepareChunkSunlight(center, chunkPos, neighbors, cornerSun.data(), 1.0f, getSunTopY, solidPadded.data());
        sunlightPrepUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
    }
    if (enableAO) {
        const auto t0 = Clock::now();
        lighting.prepareChunkAO(center, chunkPos, neighbors, cornerAO.data(), solidPadded.data());
        aoPrepUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
    }

    const MatIdLut& matIdLut = getCachedMatIdLut(atlas);


    std::array<GreedyCell, CHUNK_SIZE * CHUNK_SIZE> mask{};
    uint16_t currentMaskGen = 1;

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
            ++currentMaskGen;
            if (currentMaskGen == 0) {
                currentMaskGen = 1;
                for (GreedyCell& cell : mask) {
                    cell.gen = 0;
                }
            }

            const auto tMaskTransition0 = Clock::now();
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

                    const BlockID a = BlockID(blockGrid[gridIndex(pax, pay, paz)]);
                    const BlockID b = BlockID(blockGrid[gridIndex(pbx, pby, pbz)]);

                    if ((a != BlockID::Air) == (b != BlockID::Air))
                        continue;

                    // only emit faces for solids that belong to the center chunk.
                    // without this, two adjacent chunks can both emit the same border face.
                    const bool solidIsA = (a != BlockID::Air);
                    const int solidX = solidIsA ? pax : pbx;
                    const int solidY = solidIsA ? pay : pby;
                    const int solidZ = solidIsA ? paz : pbz;
                    if (solidX < 0 || solidX >= CHUNK_SIZE ||
                        solidY < 0 || solidY >= CHUNK_SIZE ||
                        solidZ < 0 || solidZ >= CHUNK_SIZE) {
                        continue;
                    }

                    GreedyCell& c = mask[j * CHUNK_SIZE + i];
                    c.gen = currentMaskGen;
                    c.sign = (a != BlockID::Air) ? +1 : -1;
                    c.block = (a != BlockID::Air) ? a : b;

                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);

                    c.matId = matIdLut[size_t(c.block)][face];
                    c.lightKey = 0u;
                    c.mergeKey =
                        uint64_t(uint8_t(c.block)) |
                        (uint64_t(c.sign > 0 ? 1u : 0u) << 8) |
                        (uint64_t(c.matId) << 16);
                    if (enableAO || enableShadows) {
                        c.sx = int16_t((c.sign > 0) ? (pax + dx) : pbx);
                        c.sy = int16_t((c.sign > 0) ? (pay + dy) : pby);
                        c.sz = int16_t((c.sign > 0) ? (paz + dz) : pbz);
                    }
                }
            }
            maskTransitionDur += (Clock::now() - tMaskTransition0);

            if (enableAO || enableShadows) {
                const auto tMaskLighting0 = Clock::now();
                for (int j = 0; j < CHUNK_SIZE; ++j) {
                    for (int i = 0; i < CHUNK_SIZE; ++i) {
                        GreedyCell& c = mask[j * CHUNK_SIZE + i];
                        if (c.gen != currentMaskGen) {
                            continue;
                        }

                        const int sx = int(c.sx);
                        const int sy = int(c.sy);
                        const int sz = int(c.sz);

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

                        const int ci0 = lighting.cornerIndexPadded(cx0, cy0, cz0);
                        const int ci1 = lighting.cornerIndexPadded(cx1, cy1, cz1);
                        const int ci2 = lighting.cornerIndexPadded(cx2, cy2, cz2);
                        const int ci3 = lighting.cornerIndexPadded(cx3, cy3, cz3);

                        if (enableAO) {
                            c.ao[0] = cornerAO[ci0];
                            c.ao[1] = cornerAO[ci1];
                            c.ao[2] = cornerAO[ci2];
                            c.ao[3] = cornerAO[ci3];
                        }
                        if (enableShadows) {
                            c.sun[0] = cornerSun[ci0];
                            c.sun[1] = cornerSun[ci1];
                            c.sun[2] = cornerSun[ci2];
                            c.sun[3] = cornerSun[ci3];
                        }

                        uint32_t key = 0u;
                        if (enableAO) {
                            key |= (uint32_t(c.ao[0] & 0xFu) << 0);
                            key |= (uint32_t(c.ao[1] & 0xFu) << 4);
                            key |= (uint32_t(c.ao[2] & 0xFu) << 8);
                            key |= (uint32_t(c.ao[3] & 0xFu) << 12);
                        }
                        if (enableShadows) {
                            key |= (uint32_t(c.sun[0] & 0xFu) << 16);
                            key |= (uint32_t(c.sun[1] & 0xFu) << 20);
                            key |= (uint32_t(c.sun[2] & 0xFu) << 24);
                            key |= (uint32_t(c.sun[3] & 0xFu) << 28);
                        }
                        c.lightKey = key;
                        c.mergeKey =
                            uint64_t(uint8_t(c.block)) |
                            (uint64_t(c.sign > 0 ? 1u : 0u) << 8) |
                            (uint64_t(c.matId) << 16) |
                            (uint64_t(c.lightKey) << 24);
                    }
                }
                maskLightingDur += (Clock::now() - tMaskLighting0);
            }

            const auto tEmit0 = Clock::now();
            for (int j = 0; j < CHUNK_SIZE; ++j) {
                for (int i = 0; i < CHUNK_SIZE; ) {

                    GreedyCell& c = mask[j * CHUNK_SIZE + i];
                    if (c.gen != currentMaskGen) { ++i; continue; }

                    int w = 1;
                    while (i + w < CHUNK_SIZE) {
                        GreedyCell& r = mask[j * CHUNK_SIZE + (i + w)];

                        if (r.gen != currentMaskGen || r.mergeKey != c.mergeKey) {
                            break;
                        }
                        ++w;
                    }

                    int h = 1;
                    bool stop = false;
                    while (j + h < CHUNK_SIZE && !stop) {
                        for (int k = 0; k < w; ++k) {
                            GreedyCell& r = mask[(j + h) * CHUNK_SIZE + (i + k)];
                            if (r.gen != currentMaskGen || r.mergeKey != c.mergeKey) {
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

                    for (int k = 0; k < 4; ++k)
                    {
                        uint8_t uvCorner = uvRemap[face][k];

                        vertices.push_back(packVoxelVertex(
                            vtx[k],
                            face,
                            uvCorner,
                            c.matId,
                            enableAO ? c.ao[k] : 0,
                            enableShadows ? c.sun[k] : 0
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
                            mask[(j + yy) * CHUNK_SIZE + (i + xx)].gen = 0;

                    i += w;
                }
            }
            greedyEmitDur += (Clock::now() - tEmit0);
        }
    }

    const uint64_t maskTransitionUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(maskTransitionDur).count());
    const uint64_t maskLightingUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(maskLightingDur).count());
    const uint64_t maskBuildUs = maskTransitionUs + maskLightingUs;
    const uint64_t greedyEmitUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(greedyEmitDur).count());

    const uint64_t totalUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tTotal0).count());
    g_profileChunks.fetch_add(1, std::memory_order_relaxed);
    g_profileTotalUs.fetch_add(totalUs, std::memory_order_relaxed);
    g_profileBlockGridUs.fetch_add(blockGridUs, std::memory_order_relaxed);
    g_profileSolidCacheUs.fetch_add(solidCacheUs, std::memory_order_relaxed);
    g_profileSunlightPrepUs.fetch_add(sunlightPrepUs, std::memory_order_relaxed);
    g_profileAoPrepUs.fetch_add(aoPrepUs, std::memory_order_relaxed);
    g_profileMaskTransitionUs.fetch_add(maskTransitionUs, std::memory_order_relaxed);
    g_profileMaskLightingUs.fetch_add(maskLightingUs, std::memory_order_relaxed);
    g_profileMaskBuildUs.fetch_add(maskBuildUs, std::memory_order_relaxed);
    g_profileGreedyEmitUs.fetch_add(greedyEmitUs, std::memory_order_relaxed);

    return { std::move(vertices), std::move(indices) };
}

MeshBuildProfileSnapshot ChunkMeshBuilder::getProfileSnapshot() {
    MeshBuildProfileSnapshot snap;
    snap.chunksMeshed = g_profileChunks.load(std::memory_order_relaxed);
    snap.totalUs = g_profileTotalUs.load(std::memory_order_relaxed);
    snap.blockGridUs = g_profileBlockGridUs.load(std::memory_order_relaxed);
    snap.solidCacheUs = g_profileSolidCacheUs.load(std::memory_order_relaxed);
    snap.sunlightPrepUs = g_profileSunlightPrepUs.load(std::memory_order_relaxed);
    snap.aoPrepUs = g_profileAoPrepUs.load(std::memory_order_relaxed);
    snap.maskTransitionUs = g_profileMaskTransitionUs.load(std::memory_order_relaxed);
    snap.maskLightingUs = g_profileMaskLightingUs.load(std::memory_order_relaxed);
    snap.maskBuildUs = g_profileMaskBuildUs.load(std::memory_order_relaxed);
    snap.greedyEmitUs = g_profileGreedyEmitUs.load(std::memory_order_relaxed);
    return snap;
}

void ChunkMeshBuilder::resetProfileSnapshot() {
    g_profileChunks.store(0, std::memory_order_relaxed);
    g_profileTotalUs.store(0, std::memory_order_relaxed);
    g_profileBlockGridUs.store(0, std::memory_order_relaxed);
    g_profileSolidCacheUs.store(0, std::memory_order_relaxed);
    g_profileSunlightPrepUs.store(0, std::memory_order_relaxed);
    g_profileAoPrepUs.store(0, std::memory_order_relaxed);
    g_profileMaskTransitionUs.store(0, std::memory_order_relaxed);
    g_profileMaskLightingUs.store(0, std::memory_order_relaxed);
    g_profileMaskBuildUs.store(0, std::memory_order_relaxed);
    g_profileGreedyEmitUs.store(0, std::memory_order_relaxed);
}



