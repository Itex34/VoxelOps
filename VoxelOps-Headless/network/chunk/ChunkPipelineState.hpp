#pragma once

#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

class ChunkPipelineState {
public:
    using ChunkCoord = ClientSessionManager::ChunkCoord;
    using ChunkCoordHash = ClientSessionManager::ChunkCoordHash;

    struct ChunkPipelineKey {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        ChunkCoord coord{};

        bool operator==(const ChunkPipelineKey &other) const noexcept {
            return conn == other.conn && coord == other.coord;
        }
    };

    struct ChunkPipelineKeyHash {
        std::size_t operator()(const ChunkPipelineKey &key) const noexcept {
            ChunkCoordHash chunkHash;
            const std::size_t h1 = std::hash<HSteamNetConnection>{}(key.conn);
            const std::size_t h2 = chunkHash(key.coord);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
        }
    };

    struct ChunkPrepTask {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        ChunkCoord coord{};
    };

    enum class QueuePrepResult {
        Queued,
        AlreadyQueued,
        QueueFull,
    };

    void Clear();

    QueuePrepResult QueuePrep(HSteamNetConnection conn, const ChunkCoord &coord, size_t maxQueue);
    bool WaitPopPrepTask(ChunkPrepTask &outTask, const std::atomic<bool> &quitFlag);
    void MarkPrepDoneAndQueueSend(
        const ChunkPrepTask &task,
        bool prepared,
        const std::atomic<bool> &quitFlag,
        size_t maxChunkSendQueuePerClient
    );

    bool PopNextSendChunk(HSteamNetConnection conn, ChunkCoord &outCoord);
    void PruneForClient(
        HSteamNetConnection conn, const std::unordered_set<ChunkCoord, ChunkCoordHash> &desired
    );
    size_t GetSendQueueDepthForClient(HSteamNetConnection conn) const;
    void ClearForConnection(HSteamNetConnection conn);

    void NotifyPrepWorker();
    void NotifyPrepWorkerAll();

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_chunkPrepCv;
    std::deque<ChunkPrepTask> m_chunkPrepQueue;
    std::unordered_set<ChunkPipelineKey, ChunkPipelineKeyHash> m_chunkPrepQueued;
    std::unordered_map<HSteamNetConnection, std::deque<ChunkCoord>> m_chunkSendQueues;
    std::unordered_set<ChunkPipelineKey, ChunkPipelineKeyHash> m_chunkSendQueued;
};
