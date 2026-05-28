#pragma once

#include "ChunkPipelineState.hpp"

#include "../../engine/world/ChunkManager.hpp"
#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <glm/ext/vector_int3.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

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
        HSteamNetConnection conn, const std::unordered_set<ChunkCoord, ChunkCoordHash> &desired
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

private:
    std::mutex &m_sessionMutex;
    ClientSessionManager &m_sessions;
    ChunkManager &m_chunkManager;
    ChunkPipelineState &m_pipelineState;

    uint32_t m_nextAutoUsername = 0;
    std::atomic<bool> m_chunkPrepQuit{false};
    std::thread m_chunkPrepThread;

    static constexpr size_t kMaxChunkPrepQueue = 2048;
    static constexpr size_t kMaxChunkSendQueuePerClient = 256;
};
