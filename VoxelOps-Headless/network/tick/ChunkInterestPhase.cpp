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
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "ChunkInterestPhase::RunChunkInterestPhase"
        );
        chunkInterestTasks.reserve(
            std::min<size_t>(kChunkInterestUpdatesPerLoop, m_sessions.size())
        );
        for (auto &[conn, session] : m_sessions) {
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
                break;
            }
        }
    }
    for (const ChunkInterestTask &task : chunkInterestTasks) {
        m_hooks.updateChunkStreamingForClient(task.conn, task.centerChunk, task.viewDistance);
    }
    chunkInterestUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - chunkInterestStart
    )
                                              .count());
    return chunkInterestTasks.size();
}
