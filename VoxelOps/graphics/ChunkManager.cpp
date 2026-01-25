
#include <windows.h>
#include <psapi.h>


static size_t getProcessMemoryMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return (size_t)(pmc.PrivateUsage / (1024 * 1024));
    }
    return 0;
}

#include "ChunkManager.hpp"


#include "../player/Player.hpp"
#include <glm/gtc/type_ptr.hpp>

//positions only cube used for wireframe debug
float cubeVertices[] = {
    0,0,0,  1,0,0,
    1,0,0,  1,1,0,
    1,1,0,  0,1,0,
    0,1,0,  0,0,0,

    0,0,1,  1,0,1,
    1,0,1,  1,1,1,
    1,1,1,  0,1,1,
    0,1,1,  0,0,1,

    0,0,0,  0,0,1,
    1,0,0,  1,0,1,
    1,1,0,  1,1,1,
    0,1,0,  0,1,1,
};



ChunkManager::ChunkManager(Renderer& renderer_) : renderer(renderer_){
    // wireframe/VBO/VAO
    glGenVertexArrays(1, &wireVAO);
    glGenBuffers(1, &wireVBO);
    glBindVertexArray(wireVAO);
    glBindBuffer(GL_ARRAY_BUFFER, wireVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), cubeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    debugShader.emplace("../../../../VoxelOps/shaders/debugVert.vert", "../../../../VoxelOps/shaders/debugFrag.frag");

    noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise.SetFrequency(0.009f); // controls "hilliness" (lower = smoother, higher = rougher)

    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    noise.SetSeed(std::rand());


    
    
    // build the chunk storage with positions set using unordered_map keyed by glm::ivec3
    //chunkMap.clear();
}

void ChunkManager::generateInitialChunks(int radiusChunks) {
    // Determine chunk Y range (in chunk coordinates)
    int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

    for (int x = -radiusChunks; x <= radiusChunks; ++x) {
        for (int z = -radiusChunks; z <= radiusChunks; ++z) {
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                glm::ivec3 pos = glm::ivec3(x, y, z);
                generateChunkAt(pos);
            }
        }
    }
    updateDirtyChunks();
}

void ChunkManager::generateChunkAt(const glm::ivec3& pos) {
    auto [it, inserted] = chunkMap.try_emplace(pos, pos); // forwards `pos` to Chunk::Chunk(pos)
    Chunk& chunk = it->second;

    std::random_device rd;
    std::mt19937 gen(rd());

    // ===== Terrain generation =====
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            // fractal noise
            float n = 0.0f;
            float frequency = 1.01f;
            float amplitude = 0.8f;
            float persistence = 0.5f;
            int octaves = 6;
            float maxAmplitude = 0.0f;

            for (int i = 0; i < octaves; i++) {
                n += noise.GetNoise(worldX * frequency, worldZ * frequency) * amplitude;
                maxAmplitude += amplitude;
                frequency *= 2.0f;
                amplitude *= persistence;
            }
            n /= maxAmplitude;

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.0f) * 0.5f * (WORLD_MAX_Y - WORLD_MIN_Y));

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;

                if (worldY == WORLD_MIN_Y) {
                    chunk.setBlock(x, y, z, BlockID::Bedrock);
                }
                else if (worldY < height - 2) {
                    chunk.setBlock(x, y, z, BlockID::Stone);
                }
                else if (worldY < height - 1) {
                    chunk.setBlock(x, y, z, BlockID::Dirt);
                }
                else if (worldY < height) {
                    chunk.setBlock(x, y, z, BlockID::Grass);
                }
                else {
                    chunk.setBlock(x, y, z, BlockID::Air);
                }
            }
        }
    }

    // ===== Tree placement (second pass) =====
    std::uniform_real_distribution<> chance(0.0, 1.0);
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            // find the topmost block at (x, z)
            int topY = -1;
            for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                if (chunk.getBlock(x, y, z) == BlockID::Grass) {
                    topY = y;
                    break;
                }
            }

            // if grass found, maybe spawn tree
            if (topY != -1 && chance(gen) < 0.003) { // ~0.3% chance per column
                placeTree(chunk, glm::ivec3(x, topY - 4, z), gen);
            }
        }
    }

    //  Store chunk 
    chunk.dirty = true;
}


void ChunkManager::placeTree(Chunk& chunk, const glm::ivec3& basePos, std::mt19937& gen) {
    std::uniform_int_distribution<> trunkHeightDist(10, 14);
    int trunkHeight = trunkHeightDist(gen); 

    // Trunk footprint relative offsets (2x2) - basePos is the minimum corner
    glm::ivec3 trunkOffsets[4] = {
        glm::ivec3(0, 0, 0),
        glm::ivec3(1, 0, 0),
        glm::ivec3(0, 0, 1),
        glm::ivec3(1, 0, 1)
    };

    // ===== Place 2x2 trunk =====
    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 pos(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            setBlockSafe(chunk, pos, BlockID::Log);
        }
    }

    // top of trunk (highest y coordinate used as crown base)
    int topY = basePos.y + trunkHeight - 1;

    // Crown parameters (disk-shaped)
    int crownBaseYOffset = 0;    // start at the same y as trunk top so leaves touch trunk
    int crownThickness = 2;      // 2 vertical layers (flat-ish disk)
    int crownRadius = 4;         // radius of the disk
    int topCapYOffset = crownBaseYOffset + crownThickness; // small cap above disk

    // RNG helpers
    std::uniform_real_distribution<float> holeChance(0.0f, 1.0f);
    std::uniform_real_distribution<float> strayChance(0.0f, 1.0f);

    // ===== Build the main disk: layered thin disk with inner density and sparser edges =====
    // We start from dy = crownBaseYOffset (now 0) so leaves meet the trunk top.
    for (int dy = crownBaseYOffset; dy < crownBaseYOffset + crownThickness; ++dy) {
        int layerY = topY + dy;
        for (int dx = -crownRadius; dx <= crownRadius; ++dx) {
            for (int dz = -crownRadius; dz <= crownRadius; ++dz) {
                float dist = std::sqrt(float(dx * dx + dz * dz));
                if (dist <= crownRadius + 0.25f) {
                    // radial falloff: center dense, edges sparser
                    float edgeFactor = (dist / float(crownRadius)); // 0 center -> 1 edge
                    float skipProb = glm::smoothstep(0.7f, 1.0f, edgeFactor) * 0.65f;
                    // lower layer should be a bit more solid so we reduce skipProb there
                    if (dy == crownBaseYOffset) skipProb *= 0.55f;

                    if (holeChance(gen) < skipProb) continue;

                    // center the disk over the 2x2 trunk footprint by using basePos as min corner.
                    // This keeps leaves adjacent to trunk blocks.
                    glm::ivec3 leafPos(basePos.x + dx, layerY, basePos.z + dz);
                    if (getBlockSafe(chunk, leafPos) == BlockID::Air) {
                        setBlockSafe(chunk, leafPos, BlockID::Leaves);
                    }
                }
            }
        }
    }

    // ===== Slight taper above the disk for a rounded top =====
    int taperRadius = glm::max(1, crownRadius - 2);
    int taperY = topY + topCapYOffset;
    for (int dx = -taperRadius; dx <= taperRadius; ++dx) {
        for (int dz = -taperRadius; dz <= taperRadius; ++dz) {
            float dist = std::sqrt(float(dx * dx + dz * dz));
            if (dist <= taperRadius + 0.25f) {
                glm::ivec3 leafPos(basePos.x + dx, taperY, basePos.z + dz);
                if (getBlockSafe(chunk, leafPos) == BlockID::Air) {
                    // make outer edge a little sparser
                    if (dist > (taperRadius - 0.5f) && holeChance(gen) < 0.25f) continue;
                    setBlockSafe(chunk, leafPos, BlockID::Leaves);
                }
            }
        }
    }
 


    // ===== Ensure trunk overwrites any leaves inside footprint (keeps trunk clean) =====
    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 pos(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            if (getBlockSafe(chunk, pos) != BlockID::Log) {
                setBlockSafe(chunk, pos, BlockID::Log);
            }
        }
    }
}





void ChunkManager::renderChunks(
    Shader& shader,
    Frustum& frustum,
    Player& player,
    int maxRenderDistance
)
{
    const glm::ivec3 playerChunkPos =
        worldToChunkPos(glm::ivec3(player.getPosition()));

    /* ============================
       One-time atlas setup
       ============================ */

    if (!m_tileInfoInitialized) {

        for (size_t i = 0; i < 256; ++i)
            m_tileInfo[i] = glm::vec4(0, 0, 1, 1);

        for (const auto& [name, tilePos] : atlas.tileMap) {
            int tileX = tilePos.x;
            int tileY = tilePos.y;
            int index = tileY * TEXTURE_ATLAS_SIZE + tileX;

            auto [min, max] = atlas.getUVRect(name);
            m_tileInfo[index] = glm::vec4(min, max - min);
        }

        shader.setVec4v("u_tileInfo", 256, m_tileInfo);
        shader.setFloat("u_chunkSize", float(CHUNK_SIZE));

        m_tileInfoInitialized = true;
    }

    /* ============================
       Rendering
       ============================ */

    const int maxDistSq = maxRenderDistance * maxRenderDistance;

    for (auto& [regionPos, region] : regions)
    {
        // ---- REGION FRUSTUM CULLING (GOES HERE)
        glm::vec3 regionMin =
            glm::vec3(regionPos * REGION_SIZE * CHUNK_SIZE);

        glm::vec3 regionMax =
            regionMin + glm::vec3(REGION_SIZE * CHUNK_SIZE);

        if (!frustum.isBoxVisible(regionMin, regionMax))
            continue;

        RegionMeshBuffer& gpu = *region.gpu;

        // ---- CHUNK LOOP
        for (const auto& [chunkPos, mesh] : region.chunks)
        {
            if (!mesh.valid)
                continue;

            // ---- distance culling (per-chunk)
            glm::ivec3 d = chunkPos - playerChunkPos;
            if (d.x * d.x + d.y * d.y + d.z * d.z > maxDistSq)
                continue;

            // ---- chunk frustum culling (optional but fine)
            glm::vec3 min = glm::vec3(chunkPos * CHUNK_SIZE);
            glm::vec3 max = min + glm::vec3(CHUNK_SIZE);

            if (!frustum.isBoxVisible(min, max))
                continue;

            // ---- model matrix
            glm::mat4 model(1.0f);
            model[3] = glm::vec4(min, 1.0f);
            shader.setMat4("model", model);

            // ---- draw
            gpu.drawChunkMesh(mesh);
        }
    }

}










void ChunkManager::renderChunkBorders(glm::mat4& view, glm::mat4& projection) {
    // Draw the wireframe for debugging (at Y = 0)
    debugShader->use();
    debugShader->setMat4("projection", projection);
    debugShader->setMat4("view", view);
    debugShader->setVec3("color", glm::vec3(0.0f, 1.0f, 0.0f));

    glBindVertexArray(wireVAO);

    for (int z = WORLD_MIN_Z; z <= WORLD_MAX_Z; ++z) {
        for (int x = WORLD_MIN_X; x <= WORLD_MAX_X; ++x) {
            glm::ivec3 pos(x, 0, z);
            if (!inBounds(pos)) continue;
            glm::vec3 worldPos = glm::vec3(pos.x * CHUNK_SIZE, 0.0f, pos.z * CHUNK_SIZE);
            glm::vec3 scale = glm::vec3(CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
            model = glm::scale(model, scale);
            debugShader->setMat4("model", model);
            glDrawArrays(GL_LINES, 0, 24);
        }
    }
}

std::array<bool, 6> ChunkManager::getVisibleChunkFaces(const glm::ivec3& pos) const {
    std::array<bool, 6> visibleFaces = { false, false, false, false, false, false }; // +X, -X, +Y, -Y, +Z, -Z

    const std::array<glm::ivec3, 6> directions = {
        glm::ivec3(1, 0, 0),  // +X
        glm::ivec3(-1, 0, 0), // -X
        glm::ivec3(0, 1, 0),  // +Y
        glm::ivec3(0, -1, 0), // -Y
        glm::ivec3(0, 0, 1),  // +Z
        glm::ivec3(0, 0, -1)  // -Z
    };

    for (int i = 0; i < 6; ++i) {
        glm::ivec3 neighborPos = pos + directions[i];
        auto it = chunkMap.find(neighborPos);
        if (!inBounds(neighborPos) || it == chunkMap.end() || it->second.isCompletelyAir()) {
            visibleFaces[i] = true;
        }
    }

    return visibleFaces;
}

void ChunkManager::markChunkDirty(const glm::ivec3& pos) {
    if (!inBounds(pos)) return;
    auto it = chunkMap.find(pos);
    if (it != chunkMap.end()) it->second.dirty = true;
}

void ChunkManager::updateDirtyChunks() {
    std::vector<glm::ivec3> toUpdate;

    // 1. Collect dirty chunks
    for (int y = WORLD_MIN_Y; y <= WORLD_MAX_Y; ++y) {
        for (int z = WORLD_MIN_Z; z <= WORLD_MAX_Z; ++z) {
            for (int x = WORLD_MIN_X; x <= WORLD_MAX_X; ++x) {
                glm::ivec3 pos(x, y, z);
                auto it = chunkMap.find(pos);
                if (it != chunkMap.end() && it->second.dirty)
                    toUpdate.push_back(pos);
            }
        }
    }

    // 2. Rebuild dirty chunks
    for (auto& pos : toUpdate) {
        auto it = chunkMap.find(pos);
        if (it == chunkMap.end()) continue;

        Chunk& chunk = it->second;

        auto built = builder.buildChunkMesh(
            chunk,
            pos,
            atlas,
            [&](const glm::ivec3& worldPos) -> BlockID {
                return getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
            },
            enableAO,
            enableShadows
        );

        uploadChunkMesh(pos, built.vertices, built.indices);

        chunk.dirty = false;

        // Mark neighbors dirty
        static const glm::ivec3 dirs[6] = {
            { 1, 0, 0}, {-1, 0, 0},
            { 0, 1, 0}, { 0,-1, 0},
            { 0, 0, 1}, { 0, 0,-1}
        };

        for (auto d : dirs) {
            glm::ivec3 nPos = pos + d;
            if (inBounds(nPos)) {
                auto nit = chunkMap.find(nPos);
                if (nit != chunkMap.end())
                    nit->second.dirty = true;
            }
        }
    }
}







void ChunkManager::updateChunks(const glm::ivec3& playerWorldPos, int renderDistance) {


    std::vector<glm::ivec3> toErase;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> desired;

    glm::ivec3 playerChunk = worldToChunkPos(playerWorldPos);

    int minY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxY = WORLD_MAX_Y / CHUNK_SIZE;

    for (int x = playerChunk.x - renderDistance; x <= playerChunk.x + renderDistance; ++x) {
        for (int z = playerChunk.z - renderDistance; z <= playerChunk.z + renderDistance; ++z) {
            for (int y = minY; y <= maxY; ++y) {
                desired.insert(glm::ivec3(x, y, z));
            }
        }
    }



    // --- Unload chunks too far away ---
    for (auto& [pos, chunk] : chunkMap) {
        if (desired.find(pos) == desired.end()) {
            toErase.push_back(pos);
        }
    }

    for (auto& pos : toErase) {
        chunkMap.erase(pos);
        chunkMeshes.erase(pos); // frees GPU buffers
    }

    // --- Load missing chunks ---
    for (auto& pos : desired) {
        if (chunkMap.find(pos) == chunkMap.end()) {
            generateChunkAt(pos); // generates and marks it dirty

            // also mark neighbors dirty, so shared faces get rebuilt
            static const glm::ivec3 dirs[6] = {
                { 1, 0, 0}, {-1, 0, 0},
                { 0, 1, 0}, { 0,-1, 0},
                { 0, 0, 1}, { 0, 0,-1}
            };
            for (auto d : dirs) {
                markChunkDirty(pos + d);
            }
        }
    }
}


// generate only the terrain for a single chunk and insert it into chunkMap 
void ChunkManager::generateTerrainChunkAt(const glm::ivec3& pos) {
    auto [it, inserted] = chunkMap.try_emplace(pos, pos); // forwards `pos` to Chunk::Chunk(pos)
    Chunk& chunk = it->second;

    // Per-chunk RNG is optional here; keep noise deterministic by seed (noise already seeded)
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            // fractal noise
            float n = 0.0f;
            float frequency = 1.0f;
            float amplitude = 1.9f;
            float persistence = 0.5f;
            int octaves = 6;
            float maxAmplitude = 0.0f;

            for (int i = 0; i < octaves; i++) {
                n += noise.GetNoise(worldX * frequency, worldZ * frequency) * amplitude;
                maxAmplitude += amplitude;
                frequency *= 2.0f;
                amplitude *= persistence;
            }
            if (maxAmplitude > 0.0f) n /= maxAmplitude;

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.0f) * 0.5f * (WORLD_MAX_Y - WORLD_MIN_Y));

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;

                if (worldY == WORLD_MIN_Y) {
                    chunk.setBlock(x, y, z, BlockID::Bedrock);
                }
                else if (worldY < height - 2) {
                    chunk.setBlock(x, y, z, BlockID::Stone);
                }
                else if (worldY < height - 1) {
                    chunk.setBlock(x, y, z, BlockID::Dirt);
                }
                else if (worldY < height) {
                    chunk.setBlock(x, y, z, BlockID::Grass);
                }
                else {
                    chunk.setBlock(x, y, z, BlockID::Air);
                }
            }
        }
    }

    // Insert the terrain-only chunk into chunkMap so neighbors can see it later
    chunk.dirty = true;
}


void ChunkManager::generateInitialChunks_TwoPass(int radiusChunks) {
    // convert world min/max to chunk-space (use floor for negatives)
    int minChunkY = static_cast<int>(std::floor(float(WORLD_MIN_Y) / CHUNK_SIZE));
    int maxChunkY = static_cast<int>(std::floor(float(WORLD_MAX_Y) / CHUNK_SIZE));
    int minChunkX = static_cast<int>(std::floor(float(WORLD_MIN_X) / CHUNK_SIZE));
    int maxChunkX = static_cast<int>(std::floor(float(WORLD_MAX_X) / CHUNK_SIZE));
    int minChunkZ = static_cast<int>(std::floor(float(WORLD_MIN_Z) / CHUNK_SIZE));
    int maxChunkZ = static_cast<int>(std::floor(float(WORLD_MAX_Z) / CHUNK_SIZE));

    // PASS 1: generate terrain for whole region and insert into chunkMap
    for (int x = -radiusChunks; x <= radiusChunks; ++x) {
        for (int z = -radiusChunks; z <= radiusChunks; ++z) {
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                glm::ivec3 pos(x, y, z);
                generateTerrainChunkAt(pos);
            }
        }
    }

    // PASS 2: decoration (trees) — now all chunks in region exist in chunkMap
    // Use deterministic per-chunk RNG so decoration is stable across runs (optional)
    for (auto& [pos, chunkRef] : chunkMap) {
        // Create RNG seeded by chunk position for deterministic decoration
        uint32_t seed = static_cast<uint32_t>((pos.x * 73856093u) ^ (pos.y * 19349663u) ^ (pos.z * 83492791u));
        std::mt19937 gen(seed);
        std::uniform_real_distribution<> chance(0.0, 1.0);

        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                // find topmost grass in this column (local coords)
                int topY = -1;
                for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                    if (chunkRef.getBlock(x, y, z) == BlockID::Grass) {
                        topY = y;
                        break;
                    }
                }

                if (topY != -1 && chance(gen) < 0.02) { // ~2% chance
                    placeTree(chunkRef, glm::ivec3(x, topY + 1, z), gen);
                    chunkRef.dirty = true; // mark chunk dirty so mesh will rebuild
                }
            }
        }
    }

    // Rebuild meshes for dirty chunks
    updateDirtyChunks();
}


void ChunkManager::setBlockInWorld(const glm::ivec3& worldPos, BlockID blockID) {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    if (!inBounds(chunkPos)) return;

    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) return;

    Chunk& chunk = it->second;
    chunk.setBlock(localPos.x, localPos.y, localPos.z, blockID);
    chunk.dirty = true;

    // mark neighbors if we touched an edge
    if (localPos.x == 0) markChunkDirty(chunkPos + glm::ivec3(-1, 0, 0));
    if (localPos.x == CHUNK_SIZE - 1) markChunkDirty(chunkPos + glm::ivec3(1, 0, 0));
    if (localPos.y == 0) markChunkDirty(chunkPos + glm::ivec3(0, -1, 0));
    if (localPos.y == CHUNK_SIZE - 1) markChunkDirty(chunkPos + glm::ivec3(0, 1, 0));
    if (localPos.z == 0) markChunkDirty(chunkPos + glm::ivec3(0, 0, -1));
    if (localPos.z == CHUNK_SIZE - 1) markChunkDirty(chunkPos + glm::ivec3(0, 0, 1));
}

glm::ivec3 ChunkManager::worldToChunkPos(const glm::ivec3& worldPos) const {
    // floor division to get the chunk indices (works for negatives)
    glm::vec3 f = glm::floor(glm::vec3(worldPos) / float(CHUNK_SIZE));
    return glm::ivec3(static_cast<int>(f.x), static_cast<int>(f.y), static_cast<int>(f.z));
}

glm::ivec3 ChunkManager::worldToLocalPos(const glm::ivec3& worldPos) const {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    // local = world - chunkOrigin
    glm::ivec3 local = worldPos - chunkPos * CHUNK_SIZE;
    return local;
}

bool ChunkManager::inBounds(const glm::ivec3& pos) const {
    return pos.x >= WORLD_MIN_X && pos.x <= WORLD_MAX_X &&
        pos.y >= WORLD_MIN_Y && pos.y <= WORLD_MAX_Y &&
        pos.z >= WORLD_MIN_Z && pos.z <= WORLD_MAX_Z;
}



void ChunkManager::setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id) {
    glm::ivec3 worldPos(worldX, worldY, worldZ);
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        it->second.setBlock(localPos.x, localPos.y, localPos.z, id);
        it->second.dirty = true;
    }
}

BlockID ChunkManager::getBlockGlobal(int worldX, int worldY, int worldZ) {
    glm::ivec3 worldPos(worldX, worldY, worldZ);
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        return it->second.getBlock(localPos.x, localPos.y, localPos.z);
    }
    return BlockID::Air;
}









void ChunkManager::setBlockSafe(Chunk& currentChunk, const glm::ivec3& pos, BlockID id) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE &&
        pos.y >= 0 && pos.y < CHUNK_SIZE &&
        pos.z >= 0 && pos.z < CHUNK_SIZE) {
        currentChunk.setBlock(pos.x, pos.y, pos.z, id);
    }
    else {
        // Convert local pos to world pos, then use global function
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, id);
    }
}

BlockID ChunkManager::getBlockSafe(Chunk& currentChunk, const glm::ivec3& pos) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE &&
        pos.y >= 0 && pos.y < CHUNK_SIZE &&
        pos.z >= 0 && pos.z < CHUNK_SIZE) {
        return currentChunk.getBlock(pos.x, pos.y, pos.z);
    }
    else {
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        return getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
    }
}



void ChunkManager::debugMemoryEstimate()
{
    std::cout << "---- MEMORY ESTIMATE ----\n";

    std::cout << "Process resident memory (MB): "
        << getProcessMemoryMB() << "\n";

    std::cout << "sizeof(Chunk): "
        << sizeof(Chunk) << " bytes\n";

    std::cout << "chunkMap.size(): "
        << chunkMap.size() << "\n";

    double chunkMB =
        chunkMap.size() * sizeof(Chunk) / (1024.0 * 1024.0);

    std::cout << "estimated raw chunk bytes: "
        << chunkMB << " MB\n";

    std::cout << "chunkMeshes.size(): "
        << chunkMeshes.size() << "\n";

    //// GPU stats
    //auto gpu = renderer.getGpuMeshStats();

    //std::cout << "GPU vertices used: "
    //    << gpu.usedVertexCount << " / "
    //    << gpu.totalVertexCapacity << "\n";

    //std::cout << "GPU indices used: "
    //    << gpu.usedIndexCount << " / "
    //    << gpu.totalIndexCapacity << "\n";

    //std::cout << "largest free vertex block: "
    //    << gpu.largestFreeVertexBlock << "\n";

    //std::cout << "largest free index block: "
    //    << gpu.largestFreeIndexBlock << "\n";

    //std::cout << "-------------------------\n";
}





void ChunkManager::playerBreakBlockAt(const glm::ivec3& blockCoords) {
    glm::ivec3 chunkPos = worldToChunkPos(blockCoords);
    glm::ivec3 localPos = worldToLocalPos(blockCoords);

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        it->second.removeBlock(localPos.x, localPos.y, localPos.z);
    }

    updateDirtyChunkAt(chunkPos);

    // order matches isEdgeBlock: { x==0, x==15, y==0, y==15, z==0, z==15 }
    const std::array<glm::ivec3, 6> neighborOffsets = {
        glm::ivec3(-1,  0,  0), // x==0 -> neighbor x-1
        glm::ivec3(+1,  0,  0), // x==15 -> neighbor x+1
        glm::ivec3(0, -1,  0), // y==0 -> y-1
        glm::ivec3(0, +1,  0), // y==15 -> y+1
        glm::ivec3(0,  0, -1), // z==0 -> z-1
        glm::ivec3(0,  0, +1)  // z==15 -> z+1
    };

    auto edges = isEdgeBlock(localPos);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!edges[i]) continue;
        glm::ivec3 neighborChunk = chunkPos + neighborOffsets[i];

        if (chunkMap.find(neighborChunk) != chunkMap.end()) {
            updateDirtyChunkAt(neighborChunk);
        }
    }

    debugMemoryEstimate();
}

void ChunkManager::playerPlaceBlockAt(glm::ivec3 blockCoords, int faceNormal, BlockID blockType) {
    glm::ivec3 chunkPos = worldToChunkPos(blockCoords);
    glm::ivec3 localPos = worldToLocalPos(blockCoords);


    //wall
    for (int x = 0; x < 3; x++) {
        for (int y = 0; y < 3; y++) {
            setBlockGlobal(blockCoords.x + x, blockCoords.y + y, blockCoords.z, blockType);
        }
    }

    updateDirtyChunkAt(chunkPos);


    /*
    // order matches isEdgeBlock: { x==0, x==15, y==0, y==15, z==0, z==15 }
    const std::array<glm::ivec3, 6> neighborOffsets = {
        glm::ivec3(-1,  0,  0), // x==0 -> neighbor x-1
        glm::ivec3(+1,  0,  0), // x==15 -> neighbor x+1
        glm::ivec3(0, -1,  0), // y==0 -> y-1
        glm::ivec3(0, +1,  0), // y==15 -> y+1
        glm::ivec3(0,  0, -1), // z==0 -> z-1
        glm::ivec3(0,  0, +1)  // z==15 -> z+1
    };

    auto edges = isEdgeBlock(localPos);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!edges[i]) continue;
        glm::ivec3 neighborChunk = chunkPos + neighborOffsets[i];

        if (chunkMap.find(neighborChunk) != chunkMap.end()) {
            updateDirtyChunkAt(neighborChunk);
        }
    }

    if (isCornerBlock(localPos)[0]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(-1, -1, 0));
    }

    if (isCornerBlock(localPos)[1]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(+1, +1, 0));
    }

    if (isCornerBlock(localPos)[2]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(0, -1, -1));
    }

    if (isCornerBlock(localPos)[3]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(0, +1, +1));
    }





    if (isCornerBlock(localPos)[4]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(-1, +1, 0));
    }

    if (isCornerBlock(localPos)[5]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(+1, -1, 0));
    }

    if (isCornerBlock(localPos)[6]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(0, -1, +1));
    }

    if (isCornerBlock(localPos)[7]) {
        updateDirtyChunkAt(chunkPos + glm::ivec3(0, +1, -1));
    }

    */



    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, +1, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, -1, -1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, -1, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, +1, -1));




    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, +1, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, -1, -1));



    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, +1, 0));

    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, -1, 0));

    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, -1, 0));

    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, +1, 0));




    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, 0, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, 0, -1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, 0, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, 0, -1));




    updateDirtyChunkAt(chunkPos + glm::ivec3(0, +1, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(0, -1, -1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(0, -1, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(0, +1, -1));




    updateDirtyChunkAt(chunkPos + glm::ivec3(0, 0, +1));

    updateDirtyChunkAt(chunkPos + glm::ivec3(0, 0, -1));
    
    updateDirtyChunkAt(chunkPos + glm::ivec3(+1, 0, 0));

    updateDirtyChunkAt(chunkPos + glm::ivec3(-1, 0, 0));



    updateDirtyChunkAt(chunkPos + glm::ivec3(0, +1, 0));

    updateDirtyChunkAt(chunkPos + glm::ivec3(0, -1, 0));
}



void ChunkManager::updateDirtyChunkAt(const glm::ivec3& chunkPos) {
    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) return;

    Chunk& chunk = it->second;

    auto built = builder.buildChunkMesh(
        chunk,
        chunkPos,
        atlas,
        [&](const glm::ivec3& worldPos) -> BlockID {
            return getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
        },
        enableAO,
        enableShadows
    );

    uploadChunkMesh(chunkPos, built.vertices, built.indices);

    chunk.dirty = false;
}




void ChunkManager::requestChunkRebuild(const glm::ivec3& pos) {
    auto it = chunkMap.find(pos);
    if (it == chunkMap.end()) return;

    Chunk& chunk = it->second;

    bool expected = false;
    if (!chunk.building.compare_exchange_strong(expected, true))
        return;  // already building → skip duplicate builds

    // enqueue CPU mesh build
    meshPool.enqueue([this, pos] {
        this->buildChunkMeshWorker(pos);
        });
}


void ChunkManager::buildChunkMeshWorker(glm::ivec3 pos) {

}











//region management

Region& ChunkManager::getOrCreateRegion(const glm::ivec3& chunkPos) {
    glm::ivec3 regionPos = chunkToRegionPos(chunkPos);

    auto it = regions.find(regionPos);
    if (it != regions.end()) {
        return it->second;
    }

    // Create new region
    auto [newIt, inserted] = regions.emplace(
        regionPos,
        Region(regionPos, REGION_VERTEX_BYTES, REGION_INDEX_BYTES)
    );

    std::cout << "[ChunkManager] Created region at ("
        << regionPos.x << ", " << regionPos.y << ", " << regionPos.z << ")\n";

    return newIt->second;
}




void ChunkManager::uploadChunkMesh(
    const glm::ivec3& chunkPos,
    const std::vector<VoxelVertex>& vertices,
    const std::vector<uint16_t>& indices)
{
    if (vertices.empty() || indices.empty()) return;

    Region& region = getOrCreateRegion(chunkPos);

    // Remove old mesh if exists
    auto meshIt = region.chunks.find(chunkPos);
    if (meshIt != region.chunks.end()) {
        region.gpu->destroyChunkMesh(meshIt->second);
        region.chunks.erase(meshIt);
    }

    // Create new mesh
    ChunkMesh mesh = region.gpu->createChunkMesh(vertices, indices);
    if (mesh.valid) {
        region.chunks[chunkPos] = mesh;
    }
}




void ChunkManager::removeChunkMesh(const glm::ivec3& chunkPos) {
    glm::ivec3 regionPos = chunkToRegionPos(chunkPos);

    auto regionIt = regions.find(regionPos);
    if (regionIt == regions.end()) return;

    Region& region = regionIt->second;
    auto meshIt = region.chunks.find(chunkPos);
    if (meshIt != region.chunks.end()) {
        region.gpu->destroyChunkMesh(meshIt->second);
        region.chunks.erase(meshIt);
    }

    // Optional: Remove empty regions to free GPU memory
    if (region.chunks.empty()) {
        regions.erase(regionIt);
    }
}







/*
void ChunkManager::rebuildRegion(const glm::ivec3& regionPos)
{
    auto it = regions.find(regionPos);
    if (it == regions.end()) return;

    Region& oldRegion = it->second;

    auto newGpu = std::make_unique<RegionMeshBuffer>(
        REGION_VERTEX_BYTES,
        REGION_INDEX_BYTES
    );

    std::unordered_map<glm::ivec3, ChunkMesh, IVec3Hash> newMeshes;

    for (auto& [chunkPos, oldMesh] : oldRegion.chunks) {
        Chunk& chunk = chunkMap.at(chunkPos);

        std::vector<VoxelVertex> vertices;
        std::vector<uint16_t> indices;

        builder.buildChunkMeshCPU(
            chunk,
            chunkPos,
            atlas,
            [&](const glm::ivec3& wp) {
                return getBlockGlobal(wp.x, wp.y, wp.z);
            },
            enableAO,
            enableShadows,
            vertices,
            indices
        );

        ChunkMesh mesh = newGpu->createChunkMesh(vertices, indices);
        if (!mesh.valid) {
            std::cerr << "[FATAL] Region rebuild failed\n";
            return;
        }

        newMeshes.emplace(chunkPos, mesh);
    }

    oldRegion.gpu = std::move(newGpu);
    oldRegion.chunks = std::move(newMeshes);
}
*/