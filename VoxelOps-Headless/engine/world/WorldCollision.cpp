#include "WorldCollision.hpp"

#include "ChunkManager.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <shared_mutex>

namespace {

    constexpr bool kEnableChunkMapMutexDiagnostics = false;
    constexpr int64_t kSlowChunkMapLockWaitUs = 250;
    constexpr float kCollisionSkin = 0.001f;
    std::atomic<uint64_t> g_chunkMapSlowWaitLogCount{0};

    void MaybeLogSlowChunkMapLock(const char *fn, int64_t waitUs) {
        if (!kEnableChunkMapMutexDiagnostics || waitUs < kSlowChunkMapLockWaitUs) {
            return;
        }
        const uint64_t count =
            g_chunkMapSlowWaitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 40 || (count % 200) == 0) {
            std::cerr << "[perf/chunk-map] slow lock wait fn=" << fn << " waitUs=" << waitUs
                      << " count=" << count << "\n";
        }
    }

} // namespace

namespace WorldCollision {

    QueryResult QueryAabbCollision(
        const ChunkManager &manager,
        const glm::vec3 &pos,
        float radius,
        float height,
        bool treatMissingChunkAsSolid
    ) {
        QueryResult result{};

        // Keep parity with client collision sampling: touching faces should not count as
        // penetration.
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

        const glm::ivec3 minChunkPos = manager.worldToChunkPos(glm::ivec3(ix0, iy0, iz0));
        const glm::ivec3 maxChunkPos = manager.worldToChunkPos(glm::ivec3(ix1, iy1, iz1));

        for (int cx = minChunkPos.x; cx <= maxChunkPos.x; ++cx) {
            for (int cy = minChunkPos.y; cy <= maxChunkPos.y; ++cy) {
                for (int cz = minChunkPos.z; cz <= maxChunkPos.z; ++cz) {
                    const glm::ivec3 chunkPos(cx, cy, cz);
                    if (!manager.inBounds(chunkPos)) {
                        continue;
                    }

                    const int chunkWorldMinX = cx * CHUNK_SIZE;
                    const int chunkWorldMinY = cy * CHUNK_SIZE;
                    const int chunkWorldMinZ = cz * CHUNK_SIZE;
                    const int chunkWorldMaxX = chunkWorldMinX + CHUNK_SIZE - 1;
                    const int chunkWorldMaxY = chunkWorldMinY + CHUNK_SIZE - 1;
                    const int chunkWorldMaxZ = chunkWorldMinZ + CHUNK_SIZE - 1;

                    const int localMinX = std::max(ix0, chunkWorldMinX) - chunkWorldMinX;
                    const int localMinY = std::max(iy0, chunkWorldMinY) - chunkWorldMinY;
                    const int localMinZ = std::max(iz0, chunkWorldMinZ) - chunkWorldMinZ;
                    const int localMaxX = std::min(ix1, chunkWorldMaxX) - chunkWorldMinX;
                    const int localMaxY = std::min(iy1, chunkWorldMaxY) - chunkWorldMinY;
                    const int localMaxZ = std::min(iz1, chunkWorldMaxZ) - chunkWorldMinZ;

                    const ServerChunk *chunk = nullptr;
                    {
                        const auto lockStart = std::chrono::steady_clock::now();
                        std::shared_lock<std::shared_mutex> lk(manager.mapMutex);
                        const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::steady_clock::now() - lockStart
                        )
                                                .count();
                        MaybeLogSlowChunkMapLock("queryAabbCollision.chunkFetch", waitUs);
                        chunk = manager.m_chunks.get(chunkPos);
                    }

                    if (chunk == nullptr) {
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

                    const bool hasSolid = chunk->anySolidInLocalRange(
                        localMinX, localMinY, localMinZ, localMaxX, localMaxY, localMaxZ
                    );
                    if (hasSolid) {
                        result.collided = true;
                        return result;
                    }
                }
            }
        }

        return result;
    }

} // namespace WorldCollision
