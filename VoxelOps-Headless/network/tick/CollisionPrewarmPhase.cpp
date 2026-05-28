#include "CollisionPrewarmPhase.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

CollisionPrewarmPhase::CollisionPrewarmPhase(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    ChunkManager &chunkManager
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_chunkManager(chunkManager) {}

size_t CollisionPrewarmPhase::RunCollisionPrewarmPhase(
    bool simBacklog,
    std::chrono::steady_clock::time_point &nextCollisionPrewarmAt,
    double &collisionPrewarmUs
) {
    constexpr int kCollisionPrewarmRadiusXZ = 1;
    constexpr int kCollisionPrewarmRadiusY = 1;
    constexpr size_t kMaxCollisionPrewarmGenerationsPerLoop = 8;
    constexpr int64_t kCollisionPrewarmBudgetUs = 1500;
    const auto kCollisionPrewarmInterval = std::chrono::milliseconds(50);

    collisionPrewarmUs = 0.0;
    if (simBacklog) {
        // Sim tick is behind: defer background world work and retry once backlog clears.
        nextCollisionPrewarmAt = std::chrono::steady_clock::now();
        return 0;
    }

    const auto prewarmNow = std::chrono::steady_clock::now();
    if (prewarmNow < nextCollisionPrewarmAt) {
        return 0;
    }

    const auto collisionPrewarmStart = prewarmNow;
    size_t collisionPrewarmGeneratedThisLoop = 0;
    std::vector<PlayerID> activePlayerIds;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "CollisionPrewarmPhase::RunCollisionPrewarmPhase"
        );
        activePlayerIds.reserve(m_sessions.size());
        for (const auto &[_, session] : m_sessions) {
            if (session.playerId != 0) {
                activePlayerIds.push_back(session.playerId);
            }
        }
    }

    for (PlayerID playerId : activePlayerIds) {
        if (collisionPrewarmGeneratedThisLoop >= kMaxCollisionPrewarmGenerationsPerLoop) {
            break;
        }
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - collisionPrewarmStart
        )
                                      .count();
        if (elapsedUs >= kCollisionPrewarmBudgetUs) {
            break;
        }

        const auto playerOpt = m_playerManager.getPlayerCopy(playerId);
        if (!playerOpt.has_value()) {
            continue;
        }

        const ServerPlayer &player = *playerOpt;
        const glm::ivec3 playerWorldPos(
            static_cast<int>(std::floor(player.position.x)),
            static_cast<int>(std::floor(player.position.y)),
            static_cast<int>(std::floor(player.position.z))
        );
        const glm::ivec3 centerChunk = m_chunkManager.worldToChunkPos(playerWorldPos);

        bool hitLoopBudget = false;
        for (int dx = -kCollisionPrewarmRadiusXZ; dx <= kCollisionPrewarmRadiusXZ && !hitLoopBudget;
             ++dx) {
            for (int dz = -kCollisionPrewarmRadiusXZ;
                 dz <= kCollisionPrewarmRadiusXZ && !hitLoopBudget;
                 ++dz) {
                for (int dy = -kCollisionPrewarmRadiusY; dy <= kCollisionPrewarmRadiusY; ++dy) {
                    const int64_t innerElapsedUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - collisionPrewarmStart
                        )
                            .count();
                    if (innerElapsedUs >= kCollisionPrewarmBudgetUs) {
                        hitLoopBudget = true;
                        break;
                    }

                    const glm::ivec3 chunkPos(
                        centerChunk.x + dx, centerChunk.y + dy, centerChunk.z + dz
                    );
                    if (!m_chunkManager.inBounds(chunkPos)) {
                        continue;
                    }
                    if (m_chunkManager.hasChunkLoaded(chunkPos)) {
                        continue;
                    }
                    m_chunkManager.generateTerrainChunkAt(chunkPos);
                    ++collisionPrewarmGeneratedThisLoop;
                    if (collisionPrewarmGeneratedThisLoop >=
                        kMaxCollisionPrewarmGenerationsPerLoop) {
                        hitLoopBudget = true;
                        break;
                    }
                }
            }
        }
    }
    collisionPrewarmUs =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - collisionPrewarmStart
        )
                                .count());
    nextCollisionPrewarmAt = std::chrono::steady_clock::now() + kCollisionPrewarmInterval;
    return collisionPrewarmGeneratedThisLoop;
}
