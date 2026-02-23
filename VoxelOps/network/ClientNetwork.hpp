#pragma once



#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <string>
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
    ClientNetwork();
    ~ClientNetwork();

    // Initialize GameNetworkingSockets. Call once before using other methods.
    // Returns true on success.
    bool Start();

    // Connect to server at IPv4 address + port. Returns true if a connection attempt was started.
    bool ConnectTo(const char ip[16], uint16_t port);

    // Send connect request with username. Must be called after ConnectTo (or immediately after ConnectTo).
    bool SendConnectRequest(const std::string& username);

    // Send a PlayerPosition packet (seq is a local sequence number; pos/vel in world units).
    bool SendPosition(uint32_t seq, const glm::vec3& pos, const glm::vec3& vel);
    bool SendChunkRequest(const glm::ivec3& centerChunk, uint16_t viewDistance);

    // Poll for incoming messages and run callbacks. Call this regularly (e.g. every 50-100 ms).
    // This will internally RunCallbacks() and ReceiveMessagesOnConnection() to process messages.
    void Poll();

    // Close connection and cleanup
    void Shutdown();

    // Query
    bool IsConnected() const;


    bool SendShootRequest(uint32_t clientShotId, uint32_t clientTick, uint16_t weaponId,
        const glm::vec3& pos, const glm::vec3& dir,
        uint32_t seed = 0, uint8_t inputFlags = 0);

    bool PopChunkData(ChunkData& out);
    bool PopChunkDelta(ChunkDelta& out);
    bool PopChunkUnload(ChunkUnload& out);
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

    // small internal: store last connect response state
    bool m_registered = false;

    std::mutex m_chunkQueueMutex;
    std::deque<ChunkData> m_chunkDataQueue;
    std::deque<ChunkDelta> m_chunkDeltaQueue;
    std::deque<ChunkUnload> m_chunkUnloadQueue;
};
