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
#include <vector>

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

    struct ChunkInterestBounds {
        int32_t minX = 0;
        int32_t maxX = 0;
        int32_t minY = 0;
        int32_t maxY = 0;
        int32_t minZ = 0;
        int32_t maxZ = 0;
        int32_t centerX = 0;
        int32_t centerZ = 0;
        int32_t radius = 0;

        bool contains(const ChunkCoord &coord) const noexcept {
            if (coord.x < minX || coord.x > maxX || coord.y < minY || coord.y > maxY ||
                coord.z < minZ || coord.z > maxZ) {
                return false;
            }
            const int64_t dx = static_cast<int64_t>(coord.x) - centerX;
            const int64_t dz = static_cast<int64_t>(coord.z) - centerZ;
            const int64_t radius64 = radius;
            return dx * dx + dz * dz <= radius64 * radius64;
        }
    };

    enum class QueuePrepResult {
        Queued,
        AlreadyQueued,
        QueueFull,
    };

    struct QueuePrepBatchResult {
        size_t accepted = 0;
        bool queueFull = false;
    };

    void Clear();

    QueuePrepResult QueuePrep(HSteamNetConnection conn, const ChunkCoord &coord, size_t maxQueue);
    QueuePrepBatchResult QueuePrepBatch(
        HSteamNetConnection conn,
        const std::vector<ChunkCoord> &coords,
        size_t maxQueue,
        std::vector<ChunkCoord> &acceptedCoords
    );
    QueuePrepBatchResult QueuePreparedSendBatch(
        HSteamNetConnection conn,
        const std::vector<ChunkCoord> &coords,
        size_t maxChunkSendQueuePerClient,
        std::vector<ChunkCoord> &acceptedCoords
    );
    bool WaitPopPrepTask(ChunkPrepTask &outTask, const std::atomic<bool> &quitFlag);
    void MarkPrepDoneAndQueueSend(
        const ChunkPrepTask &task,
        bool prepared,
        const std::atomic<bool> &quitFlag,
        size_t maxChunkSendQueuePerClient
    );

    bool PopNextSendChunk(HSteamNetConnection conn, ChunkCoord &outCoord);
    size_t PopNextSendChunks(
        HSteamNetConnection conn, size_t maxChunks, std::vector<ChunkCoord> &outCoords
    );
    void PruneForClient(HSteamNetConnection conn, const ChunkInterestBounds &desired);
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
