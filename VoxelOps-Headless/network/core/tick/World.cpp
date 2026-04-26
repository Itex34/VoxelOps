#include "../Runtime.hpp"

size_t Runtime::RunChunkInterestPhase(bool simBacklog, double &chunkInterestUs) {
    constexpr size_t kChunkInterestUpdatesPerLoop = 4;
    const auto kChunkInterestUpdateInterval = std::chrono::milliseconds(100);

    chunkInterestUs = 0.0;
    if (simBacklog) {
        return 0;
    }

    struct ChunkInterestTask {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        glm::ivec3 centerChunk{0};
        uint16_t viewDistance = 0;
    };
    std::vector<ChunkInterestTask> chunkInterestTasks;
    const auto chunkInterestStart = std::chrono::steady_clock::now();
    const auto chunkInterestNow = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        chunkInterestTasks.reserve(
            std::min<size_t>(kChunkInterestUpdatesPerLoop, m_clients.size()));
        for (auto &[conn, session] : m_clients) {
            if (!session.hasChunkInterest) {
                continue;
            }
            if (!session.chunkInterestDirty &&
                chunkInterestNow < session.nextChunkInterestUpdateAt) {
                continue;
            }

            chunkInterestTasks.push_back(
                ChunkInterestTask{conn, session.interestCenterChunk, session.viewDistance});
            session.chunkInterestDirty = false;
            session.nextChunkInterestUpdateAt = chunkInterestNow + kChunkInterestUpdateInterval;

            if (chunkInterestTasks.size() >= kChunkInterestUpdatesPerLoop) {
                break;
            }
        }
    }
    for (const ChunkInterestTask &task : chunkInterestTasks) {
        UpdateChunkStreamingForClient(task.conn, task.centerChunk, task.viewDistance);
    }
    chunkInterestUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - chunkInterestStart)
                                              .count());
    return chunkInterestTasks.size();
}

size_t Runtime::RunChunkSendPhase(bool simBacklog,
                                        std::chrono::steady_clock::time_point &nextChunkSendFlushAt,
                                        double &chunkSendUs) {
    constexpr size_t kChunkSendGlobalBudgetPerFlush = 8;
    constexpr size_t kChunkSendPerClientBudgetPerFlush = 4;
    const auto kChunkSendFlushInterval = std::chrono::milliseconds(16);

    chunkSendUs = 0.0;
    if (simBacklog) {
        return 0;
    }

    const auto chunkSendNow = std::chrono::steady_clock::now();
    if (chunkSendNow < nextChunkSendFlushAt) {
        return 0;
    }

    const auto chunkSendStart = std::chrono::steady_clock::now();
    const size_t chunksSentThisLoop =
        FlushChunkSendQueues(kChunkSendGlobalBudgetPerFlush, kChunkSendPerClientBudgetPerFlush);
    chunkSendUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - chunkSendStart)
                                          .count());
    nextChunkSendFlushAt = chunkSendNow + kChunkSendFlushInterval;
    return chunksSentThisLoop;
}

size_t Runtime::RunCollisionPrewarmPhase(
    bool simBacklog, std::chrono::steady_clock::time_point &nextCollisionPrewarmAt,
    double &collisionPrewarmUs) {
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
        std::lock_guard<std::mutex> lk(m_mutex);
        activePlayerIds.reserve(m_clients.size());
        for (const auto &[_, session] : m_clients) {
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
                                      std::chrono::steady_clock::now() - collisionPrewarmStart)
                                      .count();
        if (elapsedUs >= kCollisionPrewarmBudgetUs) {
            break;
        }

        const auto playerOpt = m_playerManager.getPlayerCopy(playerId);
        if (!playerOpt.has_value()) {
            continue;
        }

        const ServerPlayer &player = *playerOpt;
        const glm::ivec3 playerWorldPos(static_cast<int>(std::floor(player.position.x)),
                                        static_cast<int>(std::floor(player.position.y)),
                                        static_cast<int>(std::floor(player.position.z)));
        const glm::ivec3 centerChunk = m_chunkManager.worldToChunkPos(playerWorldPos);

        bool hitLoopBudget = false;
        for (int dx = -kCollisionPrewarmRadiusXZ; dx <= kCollisionPrewarmRadiusXZ && !hitLoopBudget;
             ++dx) {
            for (int dz = -kCollisionPrewarmRadiusXZ;
                 dz <= kCollisionPrewarmRadiusXZ && !hitLoopBudget; ++dz) {
                for (int dy = -kCollisionPrewarmRadiusY; dy <= kCollisionPrewarmRadiusY; ++dy) {
                    const int64_t innerElapsedUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - collisionPrewarmStart)
                            .count();
                    if (innerElapsedUs >= kCollisionPrewarmBudgetUs) {
                        hitLoopBudget = true;
                        break;
                    }

                    const glm::ivec3 chunkPos(centerChunk.x + dx, centerChunk.y + dy,
                                              centerChunk.z + dz);
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
                                std::chrono::steady_clock::now() - collisionPrewarmStart)
                                .count());
    nextCollisionPrewarmAt = std::chrono::steady_clock::now() + kCollisionPrewarmInterval;
    return collisionPrewarmGeneratedThisLoop;
}
