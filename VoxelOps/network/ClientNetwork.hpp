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
#include <unordered_map>
#include <glm/vec3.hpp>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "../../Shared/network/PacketType.hpp" //for packet types

#include "../../Shared/network/Packets.hpp" //for packet types

class ClientNetwork {
public:
    enum class ConnectionState : uint8_t { Disconnected = 0, Connecting = 1, Connected = 2 };

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

    bool SetClientIdentityOverride(std::string_view identity);

    // Send connect request. Server assigns the canonical username.
    bool SendConnectRequest(std::string_view requestedUsername = {});

    // Send movement input for server-authoritative simulation.
    bool SendPlayerInput(const PlayerInput &input);
    bool SendRespawnRequest();
    bool SendChunkResyncRequest(const glm::ivec3 &chunkPos);
    bool SendInventoryActionRequest(const InventoryActionRequest &request);
    bool SendBlockPlaceRequest(const BlockPlaceRequest &request);
    bool SendBlockBreakRequest(const BlockBreakRequest &request);
    bool SendChunkRequest(const glm::ivec3 &centerChunk, uint16_t viewDistance);

    void Poll();

    // Close the active server connection while keeping the networking runtime started.
    void DisconnectFromServer();

    // Close connection and cleanup
    void Shutdown();

    // Query
    bool IsConnected() const;
    ConnectionState GetConnectionState() const noexcept;
    const std::string &GetConnectionStatusText() const noexcept;
    const std::string &GetAssignedUsername() const noexcept;
    bool ShouldAutoReconnect() const noexcept;
    int GetPingMs() const noexcept;

    bool SendShootRequest(
        uint32_t clientShotId,
        uint32_t clientTick,
        uint16_t weaponId,
        const glm::vec3 &pos,
        const glm::vec3 &dir,
        uint32_t seed = 0,
        uint8_t inputFlags = 0
    );
    bool SendGrappleRequest(
        uint32_t clientGrappleId,
        uint32_t clientTick,
        const glm::vec3 &pos,
        const glm::vec3 &dir,
        uint32_t seed = 0
    );

    bool PopChunkData(ChunkData &out);
    bool PopChunkDelta(ChunkDelta &out);
    bool PopChunkUnload(ChunkUnload &out);
    bool PopPlayerSnapshot(PlayerSnapshotFrame &out);
    bool PopShootResult(ShootResult &out);
    bool PopGrappleResult(GrappleResult &out);
    bool PopInventoryActionResult(InventoryActionResult &out);
    bool PopInventorySnapshot(InventorySnapshot &out);
    bool PopWorldItemSnapshot(WorldItemSnapshot &out);
    bool PopBlockPlaceResult(BlockPlaceResult &out);
    bool PopBlockBreakResult(BlockBreakResult &out);
    bool PopKillFeedEvent(KillFeedEvent &out);
    bool PopScoreboardSnapshot(ScoreboardSnapshot &out);
    ChunkQueueDepths GetChunkQueueDepths();

private:
    struct ChunkCoordKey {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const ChunkCoordKey &other) const noexcept {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct ChunkCoordKeyHash {
        size_t operator()(const ChunkCoordKey &key) const noexcept {
            const uint64_t ux = static_cast<uint32_t>(key.x);
            const uint64_t uy = static_cast<uint32_t>(key.y);
            const uint64_t uz = static_cast<uint32_t>(key.z);
            const uint64_t h = (ux * 73856093u) ^ (uy * 19349663u) ^ (uz * 83492791u);
            return static_cast<size_t>(h);
        }
    };

    HSteamNetConnection m_conn = k_HSteamNetConnection_Invalid;
    std::atomic<bool> m_started{false};
    // helper serialization
    static void AppendUint32LE(std::vector<uint8_t> &out, uint32_t v);
    static void AppendFloatLE(std::vector<uint8_t> &out, float f);

    // handle messages received from server
    void OnMessage(const uint8_t *data, uint32_t size);
    bool EnsureClientIdentity();
    bool ShouldSendChunkResyncForOverflow(const glm::ivec3 &chunkPos);
    void PruneChunkResyncOverflowState();
    void SetConnectionStatus(ConnectionState state, std::string text, bool allowReconnect = true);

    // small internal: store last connect response state
    bool m_registered = false;
    std::string m_clientIdentity;
    std::string m_assignedUsername;
    std::string m_connectionStatus = "disconnected";
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    bool m_allowAutoReconnect = true;
    bool m_useTransientIdentity = false;
    bool m_hasEverConnectedSuccessfully = false;
    bool m_retryWithAutoAssignedUsername = false;
    std::chrono::steady_clock::time_point m_nextOverflowChunkResyncAt =
        std::chrono::steady_clock::time_point::min();
    std::unordered_map<ChunkCoordKey, std::chrono::steady_clock::time_point, ChunkCoordKeyHash>
        m_chunkResyncOverflowCooldownUntil;

    std::mutex m_inboundMutex;
    std::deque<ChunkData> m_chunkDataQueue;
    std::deque<ChunkDelta> m_chunkDeltaQueue;
    std::deque<ChunkUnload> m_chunkUnloadQueue;
    std::deque<PlayerSnapshotFrame> m_playerSnapshotQueue;
    std::deque<ShootResult> m_shootResultQueue;
    std::deque<GrappleResult> m_grappleResultQueue;
    std::deque<InventoryActionResult> m_inventoryActionResultQueue;
    std::deque<InventorySnapshot> m_inventorySnapshotQueue;
    std::deque<WorldItemSnapshot> m_worldItemSnapshotQueue;
    std::deque<BlockPlaceResult> m_blockPlaceResultQueue;
    std::deque<BlockBreakResult> m_blockBreakResultQueue;
    std::deque<KillFeedEvent> m_killFeedQueue;
    std::deque<ScoreboardSnapshot> m_scoreboardQueue;
};
