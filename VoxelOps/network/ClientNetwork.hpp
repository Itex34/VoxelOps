#pragma once



#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <atomic>
#include <deque>
#include <mutex>
#include <glm/vec3.hpp>


#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include "../../Shared/network/PacketType.hpp" //for packet types

#include "../../Shared/network/Packets.hpp" //for packet types

class ClientNetwork {
public:
    enum class ConnectionState : uint8_t {
        Disconnected = 0,
        Connecting = 1,
        Connected = 2
    };

    struct ChunkQueueDepths {
        size_t chunkData = 0;
        size_t chunkDelta = 0;
        size_t chunkUnload = 0;
    };

    struct KillFeedEvent {
        std::string killer;
        std::string victim;
        uint16_t weaponId = 0;
    };

    struct ScoreboardEntry {
        std::string username;
        uint32_t kills = 0;
        uint32_t deaths = 0;
        int pingMs = -1;
    };

    struct ScoreboardSnapshot {
        int remainingSeconds = 0;
        bool matchEnded = false;
        bool matchStarted = false;
        std::string winner;
        std::vector<ScoreboardEntry> entries;
    };

    ClientNetwork();
    ~ClientNetwork();

    // Initialize GameNetworkingSockets. Call once before using other methods.
    // Returns true on success.
    bool Start();

    // Connect to server at host/IP + port. Returns true if a connection attempt was started.
    bool ConnectTo(std::string_view host, uint16_t port);

    // Send connect request. Server assigns the canonical username.
    bool SendConnectRequest(std::string_view requestedUsername = {});

    // Send movement input for server-authoritative simulation.
    bool SendPlayerInput(const PlayerInput& input);
    bool SendRespawnRequest();
    // Legacy state packet (kept for compatibility while migrating handlers).
    bool SendPosition(uint32_t seq, const glm::vec3& pos, const glm::vec3& vel);
    bool SendChunkRequest(const glm::ivec3& centerChunk, uint16_t viewDistance);

    void Poll();

    // Close connection and cleanup
    void Shutdown();

    // Query
    bool IsConnected() const;
    ConnectionState GetConnectionState() const noexcept;
    const std::string& GetConnectionStatusText() const noexcept;
    const std::string& GetAssignedUsername() const noexcept;
    bool ShouldAutoReconnect() const noexcept;
    int GetPingMs() const noexcept;


    bool SendShootRequest(uint32_t clientShotId, uint32_t clientTick, uint16_t weaponId,
        const glm::vec3& pos, const glm::vec3& dir,
        uint32_t seed = 0, uint8_t inputFlags = 0);

    bool PopChunkData(ChunkData& out);
    bool PopChunkDelta(ChunkDelta& out);
    bool PopChunkUnload(ChunkUnload& out);
    bool PopPlayerSnapshot(PlayerSnapshotFrame& out);
    bool PopShootResult(ShootResult& out);
    bool PopKillFeedEvent(KillFeedEvent& out);
    bool PopScoreboardSnapshot(ScoreboardSnapshot& out);
    ChunkQueueDepths GetChunkQueueDepths();
private:
    HSteamNetConnection m_conn = k_HSteamNetConnection_Invalid;
    std::atomic<bool> m_started{ false };
    // helper serialization
    static void AppendUint32LE(std::vector<uint8_t>& out, uint32_t v);
    static void AppendFloatLE(std::vector<uint8_t>& out, float f);

    // helpers to read LE from incoming buffer
    static uint32_t ReadUint32LE(const uint8_t* ptr);
    static float ReadFloatLE(const uint8_t* ptr);

    // handle messages received from server
    void OnMessage(const uint8_t* data, uint32_t size);
    bool EnsureClientIdentity();
    void SetConnectionStatus(ConnectionState state, std::string text, bool allowReconnect = true);

    // small internal: store last connect response state
    bool m_registered = false;
    std::string m_clientIdentity;
    std::string m_assignedUsername;
    std::string m_connectionStatus = "disconnected";
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    bool m_allowAutoReconnect = true;
    bool m_useTransientIdentity = false;

    std::mutex m_chunkQueueMutex;
    std::deque<ChunkData> m_chunkDataQueue;
    std::deque<ChunkDelta> m_chunkDeltaQueue;
    std::deque<ChunkUnload> m_chunkUnloadQueue;
    std::deque<PlayerSnapshotFrame> m_playerSnapshotQueue;
    std::deque<ShootResult> m_shootResultQueue;
    std::deque<KillFeedEvent> m_killFeedQueue;
    std::deque<ScoreboardSnapshot> m_scoreboardQueue;
};
