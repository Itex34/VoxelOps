#include "ChunkManager.hpp"
#include "WorldGen.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>

ChunkManager::ChunkManager(uint64_t seed) : worldSeed(seed) {
    noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise.SetFrequency(0.009f); // hilliness
    // seed noise deterministically from worldSeed
    noise.SetSeed(static_cast<int>(worldSeed & 0x7FFFFFFF));
}

void ChunkManager::generateInitialChunks(int numChunks) {
    WorldGen::generateInitialChunks(*this, numChunks);
}

void ChunkManager::generateInitialChunks_TwoPass(int radiusChunks) {
    WorldGen::generateInitialChunksTwoPass(*this, radiusChunks);
}

void ChunkManager::generateChunkAt(const glm::ivec3& pos) {
    WorldGen::generateChunkAt(*this, pos);
}

void ChunkManager::generateTerrainChunkAt(const glm::ivec3& pos) {
    WorldGen::generateTerrainChunkAt(*this, pos);
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

    const int minY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);

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
        for (auto& pos : toErase) {
            chunkMap.erase(pos);
            decoratedChunks.erase(pos);
        }
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
    glm::ivec3 worldPos(worldX, worldY, worldZ);
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);
    if (!inBounds(chunkPos)) return;

    ServerChunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(chunkPos);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }

    // For cross-chunk edits during generation (tree borders), materialize terrain if absent.
    if (!chunkPtr) {
        generateTerrainChunkAt(chunkPos);
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(chunkPos);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }

    if (!chunkPtr) return;
    chunkPtr->applyEdit(localPos.x, localPos.y, localPos.z, id);
    chunkPtr->markDirty();
}

BlockID ChunkManager::getBlockGlobal(int worldX, int worldY, int worldZ) {
    glm::ivec3 wp(worldX, worldY, worldZ);
    glm::ivec3 cp = worldToChunkPos(wp);
    glm::ivec3 lp = worldToLocalPos(wp);
    if (!inBounds(cp)) return BlockID::Air;

    ServerChunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(cp);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }

    // Mirror write path behavior so neighbor reads during border decoration see real terrain.
    if (!chunkPtr) {
        generateTerrainChunkAt(cp);
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
    const int minChunkY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    return pos.x >= WORLD_MIN_X && pos.x <= WORLD_MAX_X &&
        pos.y >= minChunkY && pos.y <= maxChunkY &&
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

    bool needsDecoration = false;
    // quick path: check if present
    {
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(chunkPos);
        if (it != chunkMap.end()) {
            needsDecoration = (decoratedChunks.find(chunkPos) == decoratedChunks.end());
            if (!needsDecoration) {
                return it->second.get();
            }
        }
    }

    // Upgrade terrain-only placeholders to decorated chunks when explicitly streamed.
    if (needsDecoration) {
        WorldGen::decorateChunkAt(*this, chunkPos);
        std::lock_guard<std::mutex> lk(mapMutex);
        auto it = chunkMap.find(chunkPos);
        return (it != chunkMap.end()) ? it->second.get() : nullptr;
    }

    // Streamed chunks should include the same decoration behavior as client world generation.
    generateChunkAt(chunkPos);

    std::lock_guard<std::mutex> lk(mapMutex);
    auto it = chunkMap.find(chunkPos);
    return (it != chunkMap.end()) ? it->second.get() : nullptr;
}


