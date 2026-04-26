#include "WorldCollision.hpp"

#include "ChunkManager.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <shared_mutex>

namespace {

constexpr bool kEnableChunkMapMutexDiagnostics = false;
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

} // namespace

namespace WorldCollision {

QueryResult QueryAabbCollision(
    const ChunkManager& manager,
    const glm::vec3& pos,
    float radius,
    float height,
    bool treatMissingChunkAsSolid
) {
    QueryResult result{};

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
    std::shared_lock<std::shared_mutex> lk(manager.mapMutex);
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
                const glm::ivec3 chunkPos = manager.worldToChunkPos(worldPos);
                if (!manager.inBounds(chunkPos)) {
                    continue;
                }

                ServerChunk* chunkPtr = nullptr;
                if (hasCachedChunk && chunkPos == cachedChunkPos) {
                    chunkPtr = cachedChunk;
                } else {
                    auto it = manager.chunkMap.find(chunkPos);
                    if (it != manager.chunkMap.end()) {
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

} // namespace WorldCollision
