#pragma once

#include "ChunkPipelineState.hpp"

#include "../../engine/world/ChunkManager.hpp"
#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <glm/ext/vector_int3.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChunkStreamingService {
public:
    using ChunkCoord = ClientSessionManager::ChunkCoord;
    using ChunkCoordHash = ClientSessionManager::ChunkCoordHash;

    ChunkStreamingService(
        std::mutex &sessionMutex,
        ClientSessionManager &sessions,
        ChunkManager &chunkManager,
        ChunkPipelineState &pipelineState
    );
    ~ChunkStreamingService();

    static uint16_t ClampViewDistance(uint16_t requested);
    std::string AllocateAutoUsernameLocked(HSteamNetConnection incomingConn);
    std::string BuildDisplayNameForIdentityLocked(
        std::string_view identity, std::string_view requestedName, HSteamNetConnection incomingConn
    );

    void StartChunkPipeline();
    void StopChunkPipeline();
    bool PrepareChunkForStreaming(const ChunkCoord &coord);
    bool QueueChunkPreparation(HSteamNetConnection conn, const ChunkCoord &coord);
    size_t FlushChunkSendQueueForClient(HSteamNetConnection conn, size_t maxSends);
    size_t FlushChunkSendQueues(size_t globalBudget, size_t perClientBudget);
    void PruneChunkPipelineForClient(
        HSteamNetConnection conn, const ChunkPipelineState::ChunkInterestBounds &desired
    );
    size_t GetChunkSendQueueDepthForClient(HSteamNetConnection conn);
    void ClearChunkPipelineForConnection(HSteamNetConnection conn);

    bool SendChunkData(HSteamNetConnection conn, const ChunkCoord &coord);
    bool SendChunkUnload(HSteamNetConnection conn, const ChunkCoord &coord);
    void UpdateChunkStreamingForClient(
        HSteamNetConnection conn, const glm::ivec3 &centerChunk, uint16_t viewDistance
    );

private:
    void ChunkPrepWorkerLoop();
    bool IsConnectionSendable(HSteamNetConnection conn) const;
    void ClearClientChunkState(HSteamNetConnection conn);
    ChunkPipelineState::QueuePrepBatchResult QueueChunkPreparations(
        HSteamNetConnection conn,
        const std::vector<ChunkCoord> &coords,
        std::vector<ChunkCoord> &acceptedCoords
    );

private:
    std::mutex &m_sessionMutex;
    ClientSessionManager &m_sessions;
    ChunkManager &m_chunkManager;
    ChunkPipelineState &m_pipelineState;

    struct CachedChunkPacket {
        uint64_t version = 0;
        std::vector<uint8_t> bytes;
    };
    std::unordered_map<ChunkCoord, CachedChunkPacket, ChunkCoordHash> m_chunkPacketCache;

    mutable std::mutex m_chunkPrepareMutex;
    std::condition_variable m_chunkPrepareCv;
    std::unordered_set<ChunkCoord, ChunkCoordHash> m_chunksBeingPrepared;

    uint32_t m_nextAutoUsername = 0;
    size_t m_nextChunkSendClientIndex = 0;
    std::atomic<bool> m_chunkPrepQuit{false};
    std::vector<std::thread> m_chunkPrepThreads;

    static constexpr size_t kMaxChunkPrepQueue = 8192;
    static constexpr size_t kMaxChunkSendQueuePerClient = 768;
};
