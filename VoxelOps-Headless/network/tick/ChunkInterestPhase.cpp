#include "ChunkInterestPhase.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <algorithm>
#include <utility>

ChunkInterestPhase::ChunkInterestPhase(
    std::mutex &mutex, ClientSessionManager &sessions, Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_hooks(std::move(hooks)) {}

size_t ChunkInterestPhase::RunChunkInterestPhase(bool simBacklog, double &chunkInterestUs) {
    constexpr size_t kChunkInterestUpdatesPerLoop = 8;
    constexpr int64_t kChunkInterestBudgetUs = 5000;
    const auto kChunkInterestUpdateInterval = std::chrono::milliseconds(100);

    chunkInterestUs = 0.0;
    (void)simBacklog;

    struct ChunkInterestTask {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        glm::ivec3 centerChunk{0};
        uint16_t viewDistance = 0;
    };
    std::vector<ChunkInterestTask> chunkInterestTasks;
    const auto chunkInterestStart = std::chrono::steady_clock::now();
    const auto chunkInterestNow = std::chrono::steady_clock::now();
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "ChunkInterestPhase::RunChunkInterestPhase"
        );
        chunkInterestTasks.reserve(
            std::min<size_t>(kChunkInterestUpdatesPerLoop, m_sessions.size())
        );
        std::vector<HSteamNetConnection> conns;
        conns.reserve(m_sessions.size());
        for (const auto &[conn, _] : m_sessions) {
            conns.push_back(conn);
        }

        if (conns.empty()) {
            m_nextChunkInterestIndex = 0;
        }

        const size_t startIndex = conns.empty() ? 0 : (m_nextChunkInterestIndex % conns.size());
        size_t visited = 0;
        for (; visited < conns.size(); ++visited) {
            const size_t index = (startIndex + visited) % conns.size();
            auto sessionIt = m_sessions.find(conns[index]);
            if (sessionIt == m_sessions.end()) {
                continue;
            }

            auto &[conn, session] = *sessionIt;
            if (!session.hasChunkInterest) {
                continue;
            }
            if (!session.chunkInterestDirty &&
                chunkInterestNow < session.nextChunkInterestUpdateAt) {
                continue;
            }

            chunkInterestTasks.push_back(
                ChunkInterestTask{conn, session.interestCenterChunk, session.viewDistance}
            );
            session.chunkInterestDirty = false;
            session.nextChunkInterestUpdateAt = chunkInterestNow + kChunkInterestUpdateInterval;

            if (chunkInterestTasks.size() >= kChunkInterestUpdatesPerLoop) {
                m_nextChunkInterestIndex = (index + 1) % conns.size();
                break;
            }
        }
        if (visited >= conns.size()) {
            m_nextChunkInterestIndex = 0;
        }
    }
    size_t processedTasks = 0;
    for (size_t taskIndex = 0; taskIndex < chunkInterestTasks.size(); ++taskIndex) {
        const ChunkInterestTask &task = chunkInterestTasks[taskIndex];
        m_hooks.updateChunkStreamingForClient(task.conn, task.centerChunk, task.viewDistance);
        ++processedTasks;
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - chunkInterestStart
        )
                                      .count();
        if (elapsedUs >= kChunkInterestBudgetUs) {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_mutex, "ChunkInterestPhase::RunChunkInterestPhase.rescheduleSkipped"
            );
            for (size_t skippedIndex = taskIndex + 1; skippedIndex < chunkInterestTasks.size();
                 ++skippedIndex) {
                auto sessionIt = m_sessions.find(chunkInterestTasks[skippedIndex].conn);
                if (sessionIt != m_sessions.end()) {
                    sessionIt->second.chunkInterestDirty = true;
                    sessionIt->second.nextChunkInterestUpdateAt = chunkInterestNow;
                }
            }
            break;
        }
    }
    chunkInterestUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - chunkInterestStart
    )
                                              .count());
    return processedTasks;
}
