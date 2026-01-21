#include "ChunkManager.hpp"
#include <iostream>
#include <cmath>
#include <random>
#include <algorithm>

ChunkManager::ChunkManager(uint64_t seed) : worldSeed(seed) {
    noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise.SetFrequency(0.009f); // hilliness
    // seed noise deterministically from worldSeed
    noise.SetSeed(static_cast<int>(worldSeed & 0x7FFFFFFF));
}

void ChunkManager::generateInitialChunks(int numChunks) {
    int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

    for (int x = -numChunks; x <= numChunks; ++x)
        for (int z = -numChunks; z <= numChunks; ++z)
            for (int y = minChunkY; y <= maxChunkY; ++y)
                generateChunkAt(glm::ivec3(x, y, z));

    updateDirtyChunks();
}

void ChunkManager::generateInitialChunks_TwoPass(int radiusChunks) {
    int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

    // PASS 1: terrain-only (fast)
    for (int x = -radiusChunks; x <= radiusChunks; ++x)
        for (int z = -radiusChunks; z <= radiusChunks; ++z)
            for (int y = minChunkY; y <= maxChunkY; ++y)
                generateTerrainChunkAt(glm::ivec3(x, y, z));

    // PASS 2: decoration (trees etc.)
    // Use a snapshot of the chunk pointers so we don't hold mapMutex while decorating.
    auto snap = snapshotChunkMap();
    for (auto& [pos, chunkPtr] : snap) {
        if (!chunkPtr) continue;
        // deterministic RNG per chunk so decoration is repeatable
        uint32_t seed = static_cast<uint32_t>((pos.x * 73856093u) ^ (pos.y * 19349663u) ^ (pos.z * 83492791u) ^ static_cast<uint32_t>(worldSeed));
        std::mt19937 gen(seed);
        std::uniform_real_distribution<> chance(0.0, 1.0);

        bool anyDecoration = false;

        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                int topY = -1;
                for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                    if (chunkPtr->getBlock(x, y, z) == BlockID::Grass) {
                        topY = y;
                        break;
                    }
                }
                if (topY != -1 && chance(gen) < 0.02) {
                    // placeTree will call setBlockSafe which may touch other chunks safely
                    placeTree(*chunkPtr, glm::ivec3(x, topY + 1, z), gen);
                    anyDecoration = true;
                }
            }
        }

        if (anyDecoration) {
            // mark chunk dirty (thread-safe)
            chunkPtr->markDirty();
        }
    }

    updateDirtyChunks();
}

void ChunkManager::generateChunkAt(const glm::ivec3& pos) {
    if (!inBounds(pos)) return;

    // create chunk off-map
    auto chunk = std::make_unique<ServerChunk>(pos);

    // ===== Terrain generation (noise + simple rules) =====
    // We'll reuse logic similar to generateTerrainChunkAt but for full generation
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            float n = 0.f;
            float freq = 1.f;
            float amp = 1.f;
            float persistence = 0.5f;
            int octaves = 6;
            int maxYrange = WORLD_MAX_Y - WORLD_MIN_Y;
            float maxAmp = 0.f;

            for (int o = 0; o < octaves; ++o) {
                n += noise.GetNoise(worldX * freq, worldZ * freq) * amp;
                maxAmp += amp;
                freq *= 2.f;
                amp *= persistence;
            }
            n /= (maxAmp > 0.f ? maxAmp : 1.f);

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.f) * 0.5f * maxYrange);

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;
                if (worldY == WORLD_MIN_Y) chunk->applyEdit(x, y, z, BlockID::Bedrock);
                else if (worldY < height - 2) chunk->applyEdit(x, y, z, BlockID::Stone);
                else if (worldY < height - 1) chunk->applyEdit(x, y, z, BlockID::Dirt);
                else if (worldY < height) chunk->applyEdit(x, y, z, BlockID::Grass);
                else chunk->applyEdit(x, y, z, BlockID::Air);
            }
        }
    }

    // ===== Decoration (small chance trees) =====
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> chance(0.0, 1.0);
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int topY = -1;
            for (int y = CHUNK_SIZE - 1; y >= 0; --y)
                if (chunk->getBlock(x, y, z) == BlockID::Grass) { topY = y; break; }

            if (topY != -1 && chance(gen) < 0.003) {
                placeTree(*chunk, glm::ivec3(x, topY + 1, z), gen);
            }
        }
    }

    // insert under lock
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        chunkMap[pos] = std::move(chunk);
        chunkMap[pos]->markDirty();
    }
}

void ChunkManager::generateTerrainChunkAt(const glm::ivec3& pos) {
    if (!inBounds(pos)) return;

    auto chunk = std::make_unique<ServerChunk>(pos);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            float n = 0.f;
            float freq = 1.f;
            float amp = 1.9f;
            float persistence = 0.5f;
            int octaves = 6;
            int maxYrange = WORLD_MAX_Y - WORLD_MIN_Y;
            float maxAmp = 0.f;

            for (int o = 0; o < octaves; ++o) {
                n += noise.GetNoise(worldX * freq, worldZ * freq) * amp;
                maxAmp += amp;
                freq *= 2.f;
                amp *= persistence;
            }
            n /= (maxAmp > 0.f ? maxAmp : 1.f);

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.f) * 0.5f * maxYrange);

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;
                if (worldY == WORLD_MIN_Y) chunk->applyEdit(x, y, z, BlockID::Bedrock);
                else if (worldY < height - 2) chunk->applyEdit(x, y, z, BlockID::Stone);
                else if (worldY < height - 1) chunk->applyEdit(x, y, z, BlockID::Dirt);
                else if (worldY < height) chunk->applyEdit(x, y, z, BlockID::Grass);
                else chunk->applyEdit(x, y, z, BlockID::Air);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(mapMutex);
        chunkMap[pos] = std::move(chunk);
        chunkMap[pos]->markDirty();
    }
}

void ChunkManager::placeTree(ServerChunk& chunk, const glm::ivec3& basePos, std::mt19937& gen) {
    std::uniform_int_distribution<> trunkH(6, 10);
    int trunkHeight = trunkH(gen);

    glm::ivec3 trunkOffsets[4] = {
        glm::ivec3(0,0,0), glm::ivec3(1,0,0),
        glm::ivec3(0,0,1), glm::ivec3(1,0,1)
    };

    // trunk (use setBlockSafe so we handle chunk edges)
    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 pos(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            setBlockSafe(chunk, pos, BlockID::Log);
        }
    }

    int topY = basePos.y + trunkHeight - 1;
    int crownThickness = 2;
    int crownRadius = 4;
    std::uniform_real_distribution<float> holeChance(0.0f, 1.0f);

    for (int dy = 0; dy < crownThickness; ++dy) {
        int layerY = topY + dy;
        for (int dx = -crownRadius; dx <= crownRadius; ++dx) {
            for (int dz = -crownRadius; dz <= crownRadius; ++dz) {
                float dist = std::sqrt(float(dx * dx + dz * dz));
                if (dist <= crownRadius + 0.25f) {
                    float edgeFactor = (dist / float(crownRadius));
                    float skipProb = std::clamp((edgeFactor - 0.7f) / (1.0f - 0.7f), 0.0f, 1.0f) * 0.65f;
                    if (dy == 0) skipProb *= 0.55f;
                    if (holeChance(gen) < skipProb) continue;
                    glm::ivec3 leafPos(basePos.x + dx, layerY, basePos.z + dz);
                    if (getBlockSafe(chunk, leafPos) == BlockID::Air)
                        setBlockSafe(chunk, leafPos, BlockID::Leaves);
                }
            }
        }
    }

    // taper above
    int taperRadius = std::max(1, crownRadius - 2);
    int taperY = topY + crownThickness;
    for (int dx = -taperRadius; dx <= taperRadius; ++dx) {
        for (int dz = -taperRadius; dz <= taperRadius; ++dz) {
            float dist = std::sqrt(float(dx * dx + dz * dz));
            if (dist <= taperRadius + 0.25f) {
                glm::ivec3 leafPos(basePos.x + dx, taperY, basePos.z + dz);
                if (getBlockSafe(chunk, leafPos) == BlockID::Air) {
                    if (dist > (taperRadius - 0.5f) && holeChance(gen) < 0.25f) continue;
                    setBlockSafe(chunk, leafPos, BlockID::Leaves);
                }
            }
        }
    }

    // ensure trunk blocks overwrite leaves
    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 pos(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            if (getBlockSafe(chunk, pos) != BlockID::Log) setBlockSafe(chunk, pos, BlockID::Log);
        }
    }
}

void ChunkManager::updateDirtyChunks() {
    std::vector<ServerChunk*> toUpdate;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        for (auto& [pos, chunk] : chunkMap) {
            if (chunk->dirty()) toUpdate.push_back(chunk.get());
        }
    }

    for (auto* c : toUpdate) {
        // placeholder server-side work (lighting, visibility caches, saving, etc)
        c->clearDirty();
    }
}

void ChunkManager::updateChunks(const glm::ivec3& playerWorldPos, int renderDistance) {
    glm::ivec3 playerChunk = worldToChunkPos(playerWorldPos);
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> desired;

    int minY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxY = WORLD_MAX_Y / CHUNK_SIZE;

    for (int x = playerChunk.x - renderDistance; x <= playerChunk.x + renderDistance; ++x)
        for (int z = playerChunk.z - renderDistance; z <= playerChunk.z + renderDistance; ++z)
            for (int y = minY; y <= maxY; ++y)
                desired.insert(glm::ivec3(x, y, z));

    // unload chunks not desired
    std::vector<glm::ivec3> toErase;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        for (auto& [pos, chunk] : chunkMap)
            if (desired.find(pos) == desired.end()) toErase.push_back(pos);
        for (auto& pos : toErase) chunkMap.erase(pos);
    }

    // load missing chunks (generate off-map then insert)
    for (const auto& pos : desired) {
        // quick check with map lock
        {
            std::lock_guard<std::mutex> lk(mapMutex);
            if (chunkMap.find(pos) != chunkMap.end()) continue;
        }
        // not present -> generate synchronously (could be made async)
        generateChunkAt(pos);
    }
}

void ChunkManager::setBlockInWorld(const glm::ivec3& worldPos, BlockID id) {
    glm::ivec3 cPos = worldToChunkPos(worldPos);
    glm::ivec3 lPos = worldToLocalPos(worldPos);
    if (!inBounds(cPos)) return;

    // find chunk pointer under short lock
    ServerChunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(cPos);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }
    if (!chunkPtr) return;

    // chunk methods are thread-safe
    chunkPtr->applyEdit(lPos.x, lPos.y, lPos.z, id);
    chunkPtr->markDirty();

    // mark neighbors dirty if edge modified
    if (lPos.x == 0) markChunkDirty(cPos + glm::ivec3(-1, 0, 0));
    if (lPos.x == CHUNK_SIZE - 1) markChunkDirty(cPos + glm::ivec3(1, 0, 0));
    if (lPos.y == 0) markChunkDirty(cPos + glm::ivec3(0, -1, 0));
    if (lPos.y == CHUNK_SIZE - 1) markChunkDirty(cPos + glm::ivec3(0, 1, 0));
    if (lPos.z == 0) markChunkDirty(cPos + glm::ivec3(0, 0, -1));
    if (lPos.z == CHUNK_SIZE - 1) markChunkDirty(cPos + glm::ivec3(0, 0, 1));
}

void ChunkManager::setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id) {
    setBlockInWorld(glm::ivec3(worldX, worldY, worldZ), id);
}

BlockID ChunkManager::getBlockGlobal(int worldX, int worldY, int worldZ) {
    glm::ivec3 wp(worldX, worldY, worldZ);
    glm::ivec3 cp = worldToChunkPos(wp);
    glm::ivec3 lp = worldToLocalPos(wp);

    ServerChunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(cp);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }
    if (!chunkPtr) return BlockID::Air;
    return chunkPtr->getBlock(lp.x, lp.y, lp.z);
}

void ChunkManager::setBlockSafe(ServerChunk& currentChunk, const glm::ivec3& pos, BlockID id) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE &&
        pos.y >= 0 && pos.y < CHUNK_SIZE &&
        pos.z >= 0 && pos.z < CHUNK_SIZE) {
        currentChunk.applyEdit(pos.x, pos.y, pos.z, id);
        currentChunk.markDirty();
    }
    else {
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, id);
    }
}

BlockID ChunkManager::getBlockSafe(ServerChunk& currentChunk, const glm::ivec3& pos) {
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

glm::ivec3 ChunkManager::worldToChunkPos(const glm::ivec3& wp) const {
    return glm::ivec3(floorDiv(wp.x, CHUNK_SIZE), floorDiv(wp.y, CHUNK_SIZE), floorDiv(wp.z, CHUNK_SIZE));
}

glm::ivec3 ChunkManager::worldToLocalPos(const glm::ivec3& wp) const {
    glm::ivec3 cp = worldToChunkPos(wp);
    return wp - cp * CHUNK_SIZE;
}

bool ChunkManager::inBounds(const glm::ivec3& pos) const {
    return pos.x >= WORLD_MIN_X && pos.x <= WORLD_MAX_X &&
        pos.y >= WORLD_MIN_Y && pos.y <= WORLD_MAX_Y &&
        pos.z >= WORLD_MIN_Z && pos.z <= WORLD_MAX_Z;
}

std::array<bool, 6> ChunkManager::getVisibleChunkFaces(const glm::ivec3& pos) const {
    const std::array<glm::ivec3, 6> dirs{ glm::ivec3(1,0,0), glm::ivec3(-1,0,0), glm::ivec3(0,1,0),
                                         glm::ivec3(0,-1,0), glm::ivec3(0,0,1), glm::ivec3(0,0,-1) };
    std::array<bool, 6> visible{};
    auto snap = snapshotChunkMap();
    for (int i = 0; i < 6; ++i) {
        glm::ivec3 np = pos + dirs[i];
        if (!inBounds(np)) { visible[i] = true; continue; }
        auto it = snap.find(np);
        if (it == snap.end() || it->second->isCompletelyAir()) visible[i] = true;
        else visible[i] = false;
    }
    return visible;
}

void ChunkManager::markChunkDirty(const glm::ivec3& pos) {
    if (!inBounds(pos)) return;
    ServerChunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(pos);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }
    if (chunkPtr) chunkPtr->markDirty();
}

std::unordered_map<glm::ivec3, ServerChunk*, IVec3Hash, IVec3Eq> ChunkManager::snapshotChunkMap() const {
    std::unordered_map<glm::ivec3, ServerChunk*, IVec3Hash, IVec3Eq> snap;
    std::lock_guard<std::mutex> lk(mapMutex);
    for (const auto& kv : chunkMap) snap[kv.first] = kv.second.get();
    return snap;
}



ServerChunk* ChunkManager::getChunkIfExists(const glm::ivec3& chunkPos) const {
    std::lock_guard<std::mutex> lk(mapMutex);
    auto it = chunkMap.find(chunkPos);
    return (it != chunkMap.end()) ? it->second.get() : nullptr;
}



ServerChunk* ChunkManager::loadOrGenerateChunk(const glm::ivec3& chunkPos) {
    if (!inBounds(chunkPos)) return nullptr;

    // quick path: check if present
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(chunkPos);
        if (it != chunkMap.end()) return it->second.get();
    }

    // Not present: generate a chunk off-map (expensive work here)
    auto newChunk = std::make_unique<ServerChunk>(chunkPos);
    // fill the chunk (call your generation helper)
    generateTerrainChunkAt(chunkPos); // or generateChunkAt's internals

    // Insert under lock, but check again to avoid race with another inserter
    std::lock_guard<std::mutex> lk(mapMutex);
    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) {
        chunkMap[chunkPos] = std::move(newChunk);
        chunkMap[chunkPos]->markDirty();
        return chunkMap[chunkPos].get();
    }
    else {
        // another thread inserted meanwhile => discard ours and return theirs
        return it->second.get();
    }
}
