#pragma once
#include "ServerPlayer.hpp"
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>
#include <vector>
#include <optional>

// Forward declaration for serialization helpers
struct PlayerSnapshot;

class PlayerManager {
public:
    PlayerManager();
    ~PlayerManager() = default;

    // Called when a new connection is accepted
    PlayerID onPlayerConnect(std::shared_ptr<ConnectionHandle> conn, const glm::vec3& spawnPos);

    // Called to explicitly disconnect
    bool removePlayer(PlayerID id);

    // Called by network code when a heartbeat or data arrives to update lastHeartbeat
    bool touchHeartbeat(PlayerID id);
    bool applyAuthoritativeState(PlayerID id, const glm::vec3& position, const glm::vec3& velocity);

    // Main tick. deltaSeconds: time elapsed since last tick (use fixed timestep ideally).
    void update(double deltaSeconds);

    // Build a snapshot for sending (returns raw bytes to send to a client)
    std::vector<uint8_t> buildSnapshotFor(PlayerID recipientId);

    // Send snapshots to all players (calls connection->send). This is a convenience
    // that iterates players and uses buildSnapshotFor.
    void broadcastSnapshots();

    // Lookup
    std::optional<ServerPlayer> getPlayerCopy(PlayerID id);

    // Process client input (call from your network receive handler)
    void processClientInput(PlayerID id, const std::vector<uint8_t>& packetData);

private:
    PlayerID addPlayerInternal();

    void simulatePhysicsFor(ServerPlayer& p, double dt);
    void sendBytes(const std::shared_ptr<ConnectionHandle>& conn, const std::vector<uint8_t>& buf);

    std::unordered_map<PlayerID, ServerPlayer> playersById;
    std::list<PlayerID> playersOrder; // insertion order / iteration order

    std::mutex mtx;
    std::atomic<PlayerID> nextId{ 1 };

    // Config
    std::chrono::seconds heartbeatTimeout{ 30 };
};
