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
class ChunkManager;

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
    bool enqueuePlayerInput(PlayerID id, const PlayerInput& input);
    bool setFlyModeAllowed(PlayerID id, bool allowed);
    bool setEquippedWeapon(PlayerID id, uint16_t weaponId);
    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled();

    // Main tick. deltaSeconds: time elapsed since last tick (use fixed timestep ideally).
    void update(double deltaSeconds, ChunkManager& chunkManager);

    // Build a snapshot for sending (returns raw bytes to send to a client)
    std::vector<uint8_t> buildSnapshotFor(PlayerID recipientId, uint32_t serverTick);
    std::vector<std::vector<uint8_t>> buildSnapshotsForRecipients(
        const std::vector<PlayerID>& recipientIds,
        uint32_t serverTick
    );

    // Send snapshots to all players (calls connection->send). This is a convenience
    // that iterates players and uses buildSnapshotFor.
    void broadcastSnapshots();

    // Lookup
    std::optional<ServerPlayer> getPlayerCopy(PlayerID id);
    std::vector<ServerPlayer> getAllPlayersCopy();
    bool applyDamage(PlayerID id, float damage, float& outHealthAfter, bool& outKilled);
    bool requestRespawn(PlayerID id);

private:
    PlayerID addPlayerInternal();
    glm::vec3 chooseRespawnPositionLocked(PlayerID respawningId) const;
    void respawnPlayerLocked(ServerPlayer& player, const glm::vec3& position);

    bool checkCollision(const ServerPlayer& p, const glm::vec3& pos, ChunkManager& chunkManager) const;
    void moveAndCollide(
        ServerPlayer& p,
        const glm::vec3& delta,
        ChunkManager& chunkManager,
        bool allowStepUp
    );
    void simulatePhysicsFor(ServerPlayer& p, double dt, ChunkManager& chunkManager);
    void sendBytes(const std::shared_ptr<ConnectionHandle>& conn, const std::vector<uint8_t>& buf);

    std::unordered_map<PlayerID, ServerPlayer> playersById;
    std::list<PlayerID> playersOrder; // insertion order / iteration order

    std::mutex mtx;
    std::atomic<PlayerID> nextId{ 1 };

    // Config
    std::chrono::seconds heartbeatTimeout{ 300 };
    std::chrono::milliseconds respawnDelay{ 3000 };
};
