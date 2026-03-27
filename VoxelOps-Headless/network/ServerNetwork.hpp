#pragma once


#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <glm/vec3.hpp>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>


#include "../../Shared/network/PacketType.hpp"
#include "../../Shared/network/Packets.hpp"   
#include "../player/PlayerManager.hpp"
#include "../graphics/ChunkManager.hpp"


class ServerNetwork {
public:



    ServerNetwork();
    ~ServerNetwork();

    // non-copyable
    ServerNetwork(const ServerNetwork&) = delete;
    ServerNetwork& operator=(const ServerNetwork&) = delete;

    // Initialize the networking system and start listening on the given port.
    // Returns true on success.
    bool Start(uint16_t port = 27015);

    // Run the main server loop. Returns when stopped (e.g., via Stop() or SIGINT).
    void Run();

    // Signal server to stop. Run() will return shortly thereafter.
    void Stop();

    void SaveHistoryToFile();
    void LoadHistoryFromFile();
    void SaveAdminsToFile();
    void LoadAdminsFromFile();

    bool SetAdminByUsername(const std::string& username, bool isAdmin);
    bool IsAdminUsername(const std::string& username);
    std::vector<std::pair<std::string, bool>> GetConnectedUsers();
    std::vector<std::string> GetAdminUsernames();
    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled();



    // static pointer to the currently running instance for the callback bridge
    static ServerNetwork* s_instance;

    void BroadcastRaw(const void* data, uint32_t len, HSteamNetConnection except = k_HSteamNetConnection_Invalid);

private:
    // Internal helpers
    void MainLoop();
    void ShutdownNetworking();
    static std::string ReadStringFromPacket(const void* data, uint32_t size, size_t offset = 1);
    bool IsInboundRateLimitExceeded(HSteamNetConnection incoming, PacketType packetType, uint32_t bytes);
    void HandleConnectRequest(HSteamNetConnection incoming, const void* data, uint32_t size);
    void HandleMessagePacket(HSteamNetConnection incoming, const void* data, uint32_t size);
    void HandlePlayerInputPacket(HSteamNetConnection incoming, const void* data, uint32_t size, uint64_t& playerInputPacketsThisLoop);
    void HandleChunkRequestPacket(HSteamNetConnection incoming, const void* data, uint32_t size, uint64_t& chunkRequestPacketsThisLoop);
    void HandleShootRequestPacket(HSteamNetConnection incoming, const void* data, uint32_t size);
    void RecordLagCompFrame(uint32_t serverTick);
    void DispatchInboundPacket(
        HSteamNetConnection incoming,
        PacketType packetType,
        const void* data,
        uint32_t size,
        uint64_t& playerInputPacketsThisLoop,
        uint64_t& chunkRequestPacketsThisLoop
    );

    // Callback bridge: Steam expects a free function pointer; we implement a static
    // bridge function that calls the instance method.
    static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

private:
    struct ChunkCoord {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const ChunkCoord& other) const noexcept {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct ChunkCoordHash {
        std::size_t operator()(const ChunkCoord& c) const noexcept {
            uint64_t x = static_cast<uint32_t>(c.x);
            uint64_t y = static_cast<uint32_t>(c.y);
            uint64_t z = static_cast<uint32_t>(c.z);
            uint64_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
            return static_cast<std::size_t>(h);
        }
    };

    struct ClientSession {
        std::string identity;
        std::string username;
        PlayerID playerId = 0;
        glm::ivec3 interestCenterChunk{ 0 };
        uint16_t viewDistance = 8;
        bool hasChunkInterest = false;
        bool chunkInterestDirty = false;
        std::chrono::steady_clock::time_point nextChunkInterestUpdateAt =
            std::chrono::steady_clock::time_point::min();
        std::unordered_set<ChunkCoord, ChunkCoordHash> streamedChunks;
        // ChunkData packets queued/sent but not yet marked as streamed.
        std::unordered_map<ChunkCoord, std::chrono::steady_clock::time_point, ChunkCoordHash> pendingChunkData;
        bool isAdmin = false;
        std::chrono::steady_clock::time_point inboundRateWindowStart =
            std::chrono::steady_clock::time_point::min();
        uint32_t inboundPacketsInWindow = 0;
        uint32_t inboundBytesInWindow = 0;
        uint32_t inboundPlayerInputsInWindow = 0;
        uint32_t inboundChunkRequestsInWindow = 0;
        std::chrono::steady_clock::time_point lastAcceptedShootTime =
            std::chrono::steady_clock::time_point::min();
        uint32_t lastShootClientShotId = 0;
        bool hasLastShootClientShotId = false;
    };

    struct LagCompPlayerPose {
        glm::vec3 position{ 0.0f };
        float yaw = 0.0f;
        float height = 2.56f;
        float radius = 0.3f;
    };

    struct LagCompFrame {
        uint32_t serverTick = 0;
        std::unordered_map<PlayerID, LagCompPlayerPose> players;
    };

    struct MatchScore {
        uint32_t kills = 0;
        uint32_t deaths = 0;
    };

    static uint16_t ClampViewDistance(uint16_t requested);
    std::string AllocateAutoUsernameLocked(HSteamNetConnection incomingConn);
    std::string BuildDisplayNameForIdentityLocked(
        std::string_view identity,
        std::string_view requestedName,
        HSteamNetConnection incomingConn
    );
    void UpdateChunkStreamingForClient(HSteamNetConnection conn, const glm::ivec3& centerChunk, uint16_t viewDistance);
    bool SendChunkData(HSteamNetConnection conn, const ChunkCoord& coord);
    bool SendChunkUnload(HSteamNetConnection conn, const ChunkCoord& coord);
    bool PrepareChunkForStreaming(const ChunkCoord& coord);
    bool QueueChunkPreparation(HSteamNetConnection conn, const ChunkCoord& coord);
    size_t FlushChunkSendQueueForClient(HSteamNetConnection conn, size_t maxSends);
    size_t FlushChunkSendQueues(size_t globalBudget, size_t perClientBudget);
    size_t GetChunkSendQueueDepthForClient(HSteamNetConnection conn);
    void PruneChunkPipelineForClient(
        HSteamNetConnection conn,
        const std::unordered_set<ChunkCoord, ChunkCoordHash>& desired
    );
    void ClearChunkPipelineForConnection(HSteamNetConnection conn);
    void StartChunkPipeline();
    void StopChunkPipeline();
    void ChunkPrepWorkerLoop();

    struct ChunkPipelineKey {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        ChunkCoord coord{};

        bool operator==(const ChunkPipelineKey& other) const noexcept {
            return conn == other.conn && coord == other.coord;
        }
    };

    struct ChunkPipelineKeyHash {
        std::size_t operator()(const ChunkPipelineKey& key) const noexcept {
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

    std::atomic<bool> m_quit;
    std::atomic<bool> m_started{ false };
    std::atomic<uint32_t> m_serverTick{ 0 };
    std::deque<LagCompFrame> m_lagCompFrames;
    std::mutex m_mutex;
    std::mutex m_shutdownMutex;
    bool m_shutdownComplete = false;

    // connection -> client session
    std::unordered_map<HSteamNetConnection, ClientSession> m_clients;
    std::unordered_map<PlayerID, MatchScore> m_matchScores;
    PlayerManager m_playerManager;
    ChunkManager m_chunkManager;
    std::chrono::steady_clock::time_point m_matchStartTime = std::chrono::steady_clock::now();
    std::chrono::seconds m_matchDuration{ 600 };
    bool m_matchStarted = false;
    bool m_matchEnded = false;
    std::string m_matchWinner;

    // (username, message)
    std::vector<std::pair<std::string, std::string>> m_messageHistory;
    const char* HISTORY_FILE = "chat_history.txt";
    std::unordered_set<std::string> m_adminIdentities;
    const char* ADMINS_FILE = "admins.txt";

    HSteamNetPollGroup m_pollGroup;
    HSteamListenSocket m_listenSock;
    uint32_t m_nextAutoUsername = 0;

    static constexpr size_t kMaxChunkPrepQueue = 2048;
    static constexpr size_t kMaxChunkSendQueuePerClient = 256;
    std::atomic<bool> m_chunkPrepQuit{ false };
    std::thread m_chunkPrepThread;
    std::mutex m_chunkPipelineMutex;
    std::condition_variable m_chunkPrepCv;
    std::deque<ChunkPrepTask> m_chunkPrepQueue;
    std::unordered_set<ChunkPipelineKey, ChunkPipelineKeyHash> m_chunkPrepQueued;
    std::unordered_map<HSteamNetConnection, std::deque<ChunkCoord>> m_chunkSendQueues;
    std::unordered_set<ChunkPipelineKey, ChunkPipelineKeyHash> m_chunkSendQueued;

};
