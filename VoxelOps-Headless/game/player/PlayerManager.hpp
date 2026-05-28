#pragma once
#include "ServerPlayer.hpp"
#include "../../network/snapshots/ReplicationPlayerState.hpp"
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>
#include <vector>
#include <optional>
#include <utility>

// Forward declaration for serialization helpers
struct PlayerSnapshot;
class ChunkManager;

class PlayerManager {
public:
    using ReplicationSnapshot = std::unordered_map<PlayerID, ReplicationPlayerState>;

    PlayerManager();
    ~PlayerManager() = default;

    // Called when a new connection is accepted
    PlayerID onPlayerConnect(std::shared_ptr<ConnectionHandle> conn, const glm::vec3 &spawnPos);

    // Called to explicitly disconnect
    bool removePlayer(PlayerID id);

    // Called by network code when a heartbeat or data arrives to update lastHeartbeat
    bool touchHeartbeat(PlayerID id);
    bool enqueuePlayerInput(PlayerID id, const PlayerInput &input);
    bool setFlyModeAllowed(PlayerID id, bool allowed);
    bool setEquippedWeapon(PlayerID id, uint16_t weaponId);
    bool tryFireGrapple(
        PlayerID id,
        const glm::vec3 &origin,
        const glm::vec3 &direction,
        double nowSeconds,
        const ChunkManager &chunkManager,
        GrappleFireResult &outResult
    );
    bool releaseGrapple(PlayerID id);
    bool setGrappleReeling(PlayerID id, bool reelingIn, double nowSeconds);
    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled();

    // Main tick. deltaSeconds: time elapsed since last tick (use fixed timestep ideally).
    void update(double deltaSeconds, ChunkManager &chunkManager);

    // Build a snapshot for sending (returns raw bytes to send to a client)
    std::vector<uint8_t> buildSnapshotFor(PlayerID recipientId, uint32_t serverTick);
    std::vector<std::vector<uint8_t>>
    buildSnapshotsForRecipients(const std::vector<PlayerID> &recipientIds, uint32_t serverTick);
    void CaptureReplicationSnapshot(uint32_t serverTick);

    // Send snapshots to all players (calls connection->send). This is a convenience
    // that iterates players and uses buildSnapshotFor.
    void broadcastSnapshots();

    // Lookup
    std::optional<ServerPlayer> getPlayerCopy(PlayerID id);
    std::vector<ServerPlayer> getAllPlayersCopy();
    std::vector<ServerPlayerCombatSnapshot> getAllCombatSnapshotsCopy(bool aliveOnly = false);
    bool applyDamage(PlayerID id, float damage, float &outHealthAfter, bool &outKilled);
    bool requestRespawn(PlayerID id);
    bool applyInventoryAction(
        PlayerID id,
        const InventoryActionRequest &request,
        InventoryActionResult &outResult,
        InventorySnapshot &outSnapshot
    );
    bool getInventorySnapshot(PlayerID id, InventorySnapshot &outSnapshot);
    bool getInventorySlot(PlayerID id, uint16_t slotIndex, Slot &outSlot);
    bool appendItemsToInventory(
        PlayerID id,
        uint16_t itemId,
        uint16_t quantity,
        uint16_t &outAcceptedQuantity,
        InventorySnapshot *outSnapshot = nullptr
    );
    bool consumeItemsFromInventory(
        PlayerID id,
        const std::vector<std::pair<uint16_t, uint16_t>> &itemQuantities,
        InventorySnapshot *outSnapshot = nullptr
    );

private:
    PlayerID addPlayerInternal();

    void sendBytes(const std::shared_ptr<ConnectionHandle> &conn, const std::vector<uint8_t> &buf);

    std::unordered_map<PlayerID, ServerPlayer> playersById;
    std::list<PlayerID> playersOrder; // insertion order / iteration order

    std::mutex mtx;
    std::atomic<PlayerID> nextId{1};
    std::shared_ptr<const ReplicationSnapshot> m_replicationSnapshot;
    uint32_t m_replicationSnapshotTick = 0;

    // Config
    std::chrono::seconds heartbeatTimeout{300};
    std::chrono::milliseconds respawnDelay{3000};
};
