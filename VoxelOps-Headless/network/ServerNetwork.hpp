#pragma once


#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <glm/vec3.hpp>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>


#include "../../Shared/network/PacketType.hpp"   // for packet types
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



    // static pointer to the currently running instance for the callback bridge
    static ServerNetwork* s_instance;

    void BroadcastRaw(const void* data, uint32_t len, HSteamNetConnection except = k_HSteamNetConnection_Invalid);

private:
    // Internal helpers
    void MainLoop();
    void ShutdownNetworking();
    static std::string ReadStringFromPacket(const void* data, uint32_t size, size_t offset = 1);

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
        std::string username;
        PlayerID playerId = 0;
        glm::ivec3 interestCenterChunk{ 0 };
        uint16_t viewDistance = 8;
        bool hasChunkInterest = false;
        std::unordered_set<ChunkCoord, ChunkCoordHash> streamedChunks;
        // ChunkData packets sent but not yet ACKed by the client.
        std::unordered_map<ChunkCoord, std::chrono::steady_clock::time_point, ChunkCoordHash> pendingChunkData;
        // Payload hash (FNV-1a over ChunkData.payload) expected in ChunkAck.sequence.
        std::unordered_map<ChunkCoord, uint32_t, ChunkCoordHash> pendingChunkDataPayloadHash;
    };

    static uint16_t ClampViewDistance(uint16_t requested);
    void UpdateChunkStreamingForClient(HSteamNetConnection conn, const glm::ivec3& centerChunk, uint16_t viewDistance);
    bool SendChunkData(HSteamNetConnection conn, const ChunkCoord& coord, uint32_t* outPayloadHash = nullptr);
    bool SendChunkUnload(HSteamNetConnection conn, const ChunkCoord& coord);

    std::atomic<bool> m_quit;
    std::atomic<bool> m_started{ false };
    std::mutex m_mutex;
    std::mutex m_shutdownMutex;
    bool m_shutdownComplete = false;

    // connection -> client session
    std::unordered_map<HSteamNetConnection, ClientSession> m_clients;
    PlayerManager m_playerManager;
    ChunkManager m_chunkManager;

    // (username, message)
    std::vector<std::pair<std::string, std::string>> m_messageHistory;
    const char* HISTORY_FILE = "chat_history.txt";

    HSteamNetPollGroup m_pollGroup;
    HSteamListenSocket m_listenSock;

};
