#include "ChunkManager.hpp"
#include "WorldGen.hpp"
#include <iostream>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <shared_mutex>

namespace {
constexpr bool kEnableChunkMapMutexDiagnostics = true;
constexpr int64_t kSlowChunkMapLockWaitUs = 250;
constexpr float kCollisionSkin = 0.001f;
std::atomic<uint64_t> g_chunkMapSlowWaitLogCount{ 0 };

void MaybeLogSlowChunkMapLock(const char* fn, int64_t waitUs) {
    if (!kEnableChunkMapMutexDiagnostics || waitUs < kSlowChunkMapLockWaitUs) {
        return;
    }
    const uint64_t count = g_chunkMapSlowWaitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 40 || (count % 200) == 0) {
        std::cerr
            << "[perf/chunk-map] slow lock wait fn=" << fn
            << " waitUs=" << waitUs
            << " count=" << count << "\n";
    }
}
}

ChunkManager::ChunkManager(uint64_t seed) : worldSeed(seed) {
    noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise.SetFrequency(0.009f); // hilliness
    // seed noise deterministically from worldSeed
    noise.SetSeed(static_cast<int>(worldSeed & 0x7FFFFFFF));

    // Avoid costly hash-map rehashes while holding mapMutex on streaming spikes.
    const int minChunkY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    const size_t expectedChunkCount =
        static_cast<size_t>(WORLD_MAX_X - WORLD_MIN_X + 1) *
        static_cast<size_t>(maxChunkY - minChunkY + 1) *
        static_cast<size_t>(WORLD_MAX_Z - WORLD_MIN_Z + 1);
    chunkMap.reserve(expectedChunkCount);
    decoratedChunks.reserve(expectedChunkCount);
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
        std::shared_lock<std::shared_mutex> lk(mapMutex);
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
    const int64_t radius2 = static_cast<int64_t>(renderDistance) * static_cast<int64_t>(renderDistance);

    const int minY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);

    for (int x = playerChunk.x - renderDistance; x <= playerChunk.x + renderDistance; ++x)
    {
        const int64_t dx = static_cast<int64_t>(x - playerChunk.x);
        const int64_t dx2 = dx * dx;
        for (int z = playerChunk.z - renderDistance; z <= playerChunk.z + renderDistance; ++z)
        {
            const int64_t dz = static_cast<int64_t>(z - playerChunk.z);
            if (dx2 + dz * dz > radius2) {
                continue;
            }
            for (int y = minY; y <= maxY; ++y)
            {
                const glm::ivec3 pos(x, y, z);
                if (!inBounds(pos)) {
                    continue;
                }
                desired.insert(pos);
            }
        }
    }

    // unload chunks not desired
    std::vector<glm::ivec3> toErase;
    {
        std::lock_guard<std::shared_mutex> lk(mapMutex);
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
            std::shared_lock<std::shared_mutex> lk(mapMutex);
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
        std::shared_lock<std::shared_mutex> lk(mapMutex);
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
        std::shared_lock<std::shared_mutex> lk(mapMutex);
        auto it = chunkMap.find(chunkPos);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }

    // For cross-chunk edits during generation (tree borders), materialize terrain if absent.
    if (!chunkPtr) {
        generateTerrainChunkAt(chunkPos);
        std::shared_lock<std::shared_mutex> lk(mapMutex);
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
        const auto lockStart = std::chrono::steady_clock::now();
        std::shared_lock<std::shared_mutex> lk(mapMutex);
        const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lockStart
        ).count();
        MaybeLogSlowChunkMapLock("getBlockGlobal.lookup", waitUs);
        auto it = chunkMap.find(cp);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }

    // Mirror write path behavior so neighbor reads during border decoration see real terrain.
    if (!chunkPtr) {
        generateTerrainChunkAt(cp);
        const auto lockStart = std::chrono::steady_clock::now();
        std::shared_lock<std::shared_mutex> lk(mapMutex);
        const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lockStart
        ).count();
        MaybeLogSlowChunkMapLock("getBlockGlobal.postGenerateLookup", waitUs);
        auto it = chunkMap.find(cp);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }

    if (!chunkPtr) return BlockID::Air;
    return chunkPtr->getBlock(lp.x, lp.y, lp.z);
}

bool ChunkManager::hasChunkLoaded(const glm::ivec3& chunkPos) const {
    const auto lockStart = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lk(mapMutex);
    const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - lockStart
    ).count();
    MaybeLogSlowChunkMapLock("hasChunkLoaded", waitUs);
    return chunkMap.find(chunkPos) != chunkMap.end();
}

ChunkManager::AabbCollisionQueryResult ChunkManager::queryAabbCollision(
    const glm::vec3& pos,
    float radius,
    float height,
    bool treatMissingChunkAsSolid
) const {
    AabbCollisionQueryResult result{};

    // Keep parity with client collision sampling: touching faces should not count as penetration.
    const float minX = pos.x - radius + kCollisionSkin;
    const float maxX = pos.x + radius - kCollisionSkin;
    const float minY = pos.y + kCollisionSkin;
    const float maxY = pos.y + height - kCollisionSkin;
    const float minZ = pos.z - radius + kCollisionSkin;
    const float maxZ = pos.z + radius - kCollisionSkin;

    const int ix0 = static_cast<int>(std::floor(minX));
    const int iy0 = static_cast<int>(std::floor(minY));
    const int iz0 = static_cast<int>(std::floor(minZ));
    const int ix1 = static_cast<int>(std::floor(maxX));
    const int iy1 = static_cast<int>(std::floor(maxY));
    const int iz1 = static_cast<int>(std::floor(maxZ));

    const auto lockStart = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lk(mapMutex);
    const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - lockStart
    ).count();
    MaybeLogSlowChunkMapLock("queryAabbCollision.scan", waitUs);

    glm::ivec3 cachedChunkPos(0);
    ServerChunk* cachedChunk = nullptr;
    bool hasCachedChunk = false;

    for (int x = ix0; x <= ix1; ++x) {
        for (int y = iy0; y <= iy1; ++y) {
            for (int z = iz0; z <= iz1; ++z) {
                const glm::ivec3 worldPos(x, y, z);
                const glm::ivec3 chunkPos = worldToChunkPos(worldPos);
                if (!inBounds(chunkPos)) {
                    continue;
                }

                ServerChunk* chunkPtr = nullptr;
                if (hasCachedChunk && chunkPos == cachedChunkPos) {
                    chunkPtr = cachedChunk;
                }
                else {
                    auto it = chunkMap.find(chunkPos);
                    if (it != chunkMap.end()) {
                        chunkPtr = it->second.get();
                    }
                    cachedChunkPos = chunkPos;
                    cachedChunk = chunkPtr;
                    hasCachedChunk = true;
                }

                if (!chunkPtr) {
                    if (!result.missingChunk) {
                        result.missingChunk = true;
                        result.firstMissingChunk = chunkPos;
                    }
                    if (treatMissingChunkAsSolid) {
                        result.collided = true;
                        return result;
                    }
                    continue;
                }

                const glm::ivec3 localPos = worldPos - chunkPos * CHUNK_SIZE;
                if (chunkPtr->getBlock(localPos.x, localPos.y, localPos.z) != BlockID::Air) {
                    result.collided = true;
                    return result;
                }
            }
        }
    }

    return result;
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
        std::shared_lock<std::shared_mutex> lk(mapMutex);
        auto it = chunkMap.find(pos);
        if (it != chunkMap.end()) chunkPtr = it->second.get();
    }
    if (chunkPtr) chunkPtr->markDirty();
}

std::unordered_map<glm::ivec3, ServerChunk*, IVec3Hash, IVec3Eq> ChunkManager::snapshotChunkMap() const {
    std::unordered_map<glm::ivec3, ServerChunk*, IVec3Hash, IVec3Eq> snap;
    std::shared_lock<std::shared_mutex> lk(mapMutex);
    for (const auto& kv : chunkMap) snap[kv.first] = kv.second.get();
    return snap;
}



ServerChunk* ChunkManager::getChunkIfExists(const glm::ivec3& chunkPos) const {
    const auto lockStart = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lk(mapMutex);
    const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - lockStart
    ).count();
    MaybeLogSlowChunkMapLock("getChunkIfExists", waitUs);
    auto it = chunkMap.find(chunkPos);
    return (it != chunkMap.end()) ? it->second.get() : nullptr;
}



ServerChunk* ChunkManager::loadOrGenerateChunk(const glm::ivec3& chunkPos) {
    if (!inBounds(chunkPos)) return nullptr;

    bool needsDecoration = false;
    // quick path: check if present
    {
        const auto lockStart = std::chrono::steady_clock::now();
        std::shared_lock<std::shared_mutex> lk(mapMutex);
        const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lockStart
        ).count();
        MaybeLogSlowChunkMapLock("loadOrGenerateChunk.initialLookup", waitUs);
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
        const auto lockStart = std::chrono::steady_clock::now();
        std::shared_lock<std::shared_mutex> lk(mapMutex);
        const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lockStart
        ).count();
        MaybeLogSlowChunkMapLock("loadOrGenerateChunk.postDecorateLookup", waitUs);
        auto it = chunkMap.find(chunkPos);
        return (it != chunkMap.end()) ? it->second.get() : nullptr;
    }

    // Streamed chunks should include the same decoration behavior as client world generation.
    generateChunkAt(chunkPos);

    const auto lockStart = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lk(mapMutex);
    const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - lockStart
    ).count();
    MaybeLogSlowChunkMapLock("loadOrGenerateChunk.postGenerateLookup", waitUs);
    auto it = chunkMap.find(chunkPos);
    return (it != chunkMap.end()) ? it->second.get() : nullptr;
}


