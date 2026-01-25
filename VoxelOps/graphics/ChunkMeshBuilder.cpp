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







inline uint32_t quantizePos5(float v, float chunkSize) {
    float t = v / chunkSize;                 // 0..1
    t = std::fminf(std::fmaxf(t, 0.0f), 1.0f);
    // map to 0..31 using floor (stable)
    return static_cast<uint32_t>(std::floor(t * 31.0f)) & 0x1Fu;
}










inline VoxelVertex packVoxelVertex(
    const glm::vec3& posLocal, // local pos in [0, chunkSize]
    float chunkSize,           // e.g. 16.0f
    uint8_t face,              // 0..5
    uint8_t corner,            // 0..3
    uint8_t matId,             // 0..63
    float ao_f,                // 0..1
    float sun_f               // 0..1

) {
    // Fast clamp-to-int in range [0 .. chunkSize]
    auto clampToCorner = [&](float v) -> uint32_t {
        if (v <= 0.0f) return 0u;
        if (v >= chunkSize) return static_cast<uint32_t>(chunkSize);
        uint32_t iv = static_cast<uint32_t>(v + 0.5f);
        if (iv > static_cast<uint32_t>(chunkSize)) iv = static_cast<uint32_t>(chunkSize);
        return iv & 0x1Fu;
        };

    uint32_t qx = clampToCorner(posLocal.x);
    uint32_t qy = clampToCorner(posLocal.y);
    uint32_t qz = clampToCorner(posLocal.z);

    uint32_t face_u = face & 0x7u;
    uint32_t corner_u = corner & 0x3u;
    uint32_t mat_u = matId & 0xFFu;

    auto to4bit = [](float f) -> uint32_t {
        if (f <= 0.0f) return 0u;
        if (f >= 1.0f) return 15u;
        uint32_t v = static_cast<uint32_t>(f * 15.0f + 0.5f);
        return (v > 15u) ? 15u : v;
        };

    uint32_t ao_u = to4bit(ao_f);
    uint32_t sun_u = to4bit(sun_f);

    // ----- LOW 32 bits (unchanged) -----
    uint32_t low =
        (qx & 0x1Fu)
        | ((qy & 0x1Fu) << 5)
        | ((qz & 0x1Fu) << 10)
        | ((face_u & 0x7u) << 15)
        | ((corner_u & 0x3u) << 18)
        | ((ao_u & 0xFu) << 26);

    // ----- HIGH 32 bits -----
    // bits 0–7   : matId
    // bits 8–11  : sun
    // bits 12–31 : unused

    uint32_t high =
        (mat_u << 0)
        | (sun_u << 8);

    return VoxelVertex{ low, high };
}











BuiltChunkMesh ChunkMeshBuilder::buildChunkMesh(
    const Chunk& chunk,
    const glm::ivec3& chunkPos,
    const TextureAtlas& atlas,
    BlockGetter getBlock,
    bool enableAO,
    bool enableShadows)
{
    std::vector<VoxelVertex> vertices;
    std::vector<unsigned short> indices;
    unsigned short indexOffset = 0;

    // ------------------------------------------------------------
    // Lighting (padded, Lighting owns buffer sizes)
    // ------------------------------------------------------------
    Lighting lighting(CHUNK_SIZE);
    std::vector<float> cornerSun;
    std::vector<uint8_t> cornerAO;


    if (enableShadows) {
        lighting.prepareChunkSunlight(chunk, chunkPos, getBlock, cornerSun, 1.0f);
    }

    if (enableAO) { 
        lighting.prepareChunkAO(chunkPos, getBlock, cornerAO);
    }

    const float chunkSizeF = float(CHUNK_SIZE);

    // ------------------------------------------------------------
    // Greedy meshing
    // ------------------------------------------------------------
    std::vector<GreedyCell> mask(CHUNK_SIZE * CHUNK_SIZE);

    for (int d = 0; d < 3; ++d) {
        int u = (d + 1) % 3;
        int v = (d + 2) % 3;

        glm::ivec3 eU(0), eV(0);
        eU[u] = 1;
        eV[v] = 1;

        // Sweep planes
        for (int s = 0; s <= CHUNK_SIZE; ++s) {
            std::fill(mask.begin(), mask.end(), GreedyCell{});

            // --------------------------------------------------
            // Build mask
            // --------------------------------------------------
            for (int j = 0; j < CHUNK_SIZE; ++j) {
                for (int i = 0; i < CHUNK_SIZE; ++i) {

                    glm::ivec3 pa(0), pb(0);
                    pa[u] = i; pa[v] = j; pa[d] = s - 1;
                    pb[u] = i; pb[v] = j; pb[d] = s;


                    bool paOutside =
                        pa.x < 0 || pa.y < 0 || pa.z < 0 ||
                        pa.x >= CHUNK_SIZE || pa.y >= CHUNK_SIZE || pa.z >= CHUNK_SIZE;

                    bool pbOutside =
                        pb.x < 0 || pb.y < 0 || pb.z < 0 ||
                        pb.x >= CHUNK_SIZE || pb.y >= CHUNK_SIZE || pb.z >= CHUNK_SIZE;






                    glm::ivec3 wa = chunkPos * CHUNK_SIZE + pa;
                    glm::ivec3 wb = chunkPos * CHUNK_SIZE + pb;

                    BlockID a = getBlock(wa);
                    BlockID b = getBlock(wb);



                    // If both are solid or both are air → no face
                    if ((a != BlockID::Air) == (b != BlockID::Air))
                        continue;




                    GreedyCell& c = mask[j * CHUNK_SIZE + i];
                    c.valid = true;
                    c.sign = (a != BlockID::Air) ? +1 : -1;



                    c.block = (a != BlockID::Air) ? a : b;

                    // Face index
                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);

                    // Material ID
                    auto tex = getTexCoordsForFace(c.block, face, atlas);
                    glm::vec2 tl = tex[0];
                    glm::vec2 br = tex[2];

                    float tileW = br.x - tl.x;
                    float tileH = br.y - tl.y;
                    int gridX = int(std::round(1.0f / tileW));
                    int tx = int(std::round(tl.x / tileW));
                    int ty = int(std::round(tl.y / tileH));
                    c.matId = uint8_t(ty * gridX + tx);

                    // AO / Sunlight
                    if (enableAO || enableShadows) {


                        // Pick the correct solid voxel based on sign so lighting is sampled from
                        // the same side of the plane that produced the geometry.
                        glm::ivec3 solid;
                        if (c.sign > 0) {
                            // 'a' (pa) was solid → face is on the +normal side of pa (plane at pa + 1)
                            solid = pa;
                            solid[d] += 1;
                        }
                        else {
                            // 'b' (pb) was solid → face is on the -normal side of pb (plane at pb)
                            solid = pb;
                            // no +1 offset here
                        }


                        glm::ivec3 du_i = eU;
                        glm::ivec3 dv_i = eV;

                        // These corners now belong unambiguously to the solid voxel face
                        glm::ivec3 corners[4] = {
                            solid,
                            solid + du_i,
                            solid + du_i + dv_i,
                            solid + dv_i
                        };


         
                        for (int k = 0; k < 4; ++k) {
                            if (enableAO) {
                                int ci = lighting.cornerIndexPadded(
                                    corners[k].x,
                                    corners[k].y,
                                    corners[k].z);
                                c.ao[k] = cornerAO[ci]; 
                            }

                            if (enableShadows) {
                                int ci = lighting.cornerIndexPadded(
                                    corners[k].x,
                                    corners[k].y,
                                    corners[k].z);
                                c.sun[k] = uint8_t(cornerSun[ci] * 15.f + 0.5f);
                            }
                        }

                    }


                }
            }

            // --------------------------------------------------
            // Greedy merge + emit
            // --------------------------------------------------
            for (int j = 0; j < CHUNK_SIZE; ++j) {
                for (int i = 0; i < CHUNK_SIZE; ) {

                    GreedyCell& c = mask[j * CHUNK_SIZE + i];
                    if (!c.valid) { ++i; continue; }

                    int w = 1;
                    while (i + w < CHUNK_SIZE) {
                        auto& r = mask[j * CHUNK_SIZE + (i + w)];
                        if (!r.valid || r.sign != c.sign ||
                            r.block != c.block || r.matId != c.matId ||
                            memcmp(r.ao, c.ao, 4) != 0 ||
                            memcmp(r.sun, c.sun, 4) != 0)
                            break;
                        ++w;
                    }

                    int h = 1;
                    bool stop = false;
                    while (j + h < CHUNK_SIZE && !stop) {
                        for (int k = 0; k < w; ++k) {
                            auto& r = mask[(j + h) * CHUNK_SIZE + (i + k)];
                            if (!r.valid || r.sign != c.sign ||
                                r.block != c.block || r.matId != c.matId ||
                                memcmp(r.ao, c.ao, 4) != 0 ||
                                memcmp(r.sun, c.sun, 4) != 0) {
                                stop = true;
                                break;
                            }
                        }
                        if (!stop) ++h;
                    }

                    glm::ivec3 origin(0);
                    origin[u] = i;
                    origin[v] = j;
                    origin[d] = s;

                    glm::vec3 du = glm::vec3(eU) * float(w);
                    glm::vec3 dv = glm::vec3(eV) * float(h);

                    glm::vec3 v[4];
                    v[0] = glm::vec3(origin);          // (0,0)
                    v[1] = v[0] + du;                  // (1,0)
                    v[2] = v[1] + dv;                  // (1,1)
                    v[3] = v[0] + dv;                  // (0,1)


                    int face =
                        (d == 0) ? (c.sign > 0 ? 0 : 1) :
                        (d == 1) ? (c.sign > 0 ? 2 : 3) :
                        (c.sign > 0 ? 4 : 5);



                    for (int k = 0; k < 4; ++k) {
                        uint8_t uvCorner = uvRemap[face][k];

                        vertices.push_back(packVoxelVertex(
                            v[k],
                            chunkSizeF,
                            face,
                            uvCorner,
                            c.matId,
                            c.ao[k] / 15.f,
                            c.sun[k] / 15.f
                        ));

                    }


                    if (c.sign > 0) {
                        // normal winding
                        indices.push_back(indexOffset + 0);
                        indices.push_back(indexOffset + 1);
                        indices.push_back(indexOffset + 2);
                        indices.push_back(indexOffset + 0);
                        indices.push_back(indexOffset + 2);
                        indices.push_back(indexOffset + 3);
                    }
                    else {
                        // flipped winding
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


