#include "PlayerManager.hpp"

#include "PlayerLifecycle.hpp"
#include "PlayerUpdate.hpp"
#include "inventory/PlayerInventory.hpp"

#include "../combat/PlayerCombat.hpp"
#include "../../network/snapshots/PlayerSnapshots.hpp"
#include "../../network/core/LockWaitTelemetry.hpp"
#include "../player/ServerMovementSimulation.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../../Shared/items/Items.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <utility>

namespace {
    constexpr int64_t kSlowPlayerManagerUpdateUs = 4000;
    std::atomic<uint64_t> g_playerManagerSlowUpdateCount{0};
    std::atomic<bool> g_enablePlayerManagerPerfDiagnostics{false};

    bool inventoryHasItem(const Inventory &inventory, uint16_t itemId) {
        for (const Slot &slot : inventory.slots()) {
            if (slot.itemId == itemId && slot.quantity > 0) {
                return true;
            }
        }
        return false;
    }
} // namespace

PlayerManager::PlayerManager()
    : m_replicationSnapshot(std::make_shared<ReplicationSnapshot>())
    , m_replicationSnapshotTick(0) {}

void PlayerManager::SetDebugLoggingEnabled(bool enabled) {
    g_enablePlayerManagerPerfDiagnostics.store(enabled, std::memory_order_release);
    ServerMovementSimulation::setMissingChunkCollisionDiagnosticsEnabled(enabled);
}

bool PlayerManager::IsDebugLoggingEnabled() {
    return g_enablePlayerManagerPerfDiagnostics.load(std::memory_order_acquire) ||
           ServerMovementSimulation::isMissingChunkCollisionDiagnosticsEnabled();
}

PlayerID PlayerManager::addPlayerInternal() {
    return nextId.fetch_add(1, std::memory_order_relaxed);
}

bool PlayerManager::requestRespawn(PlayerID id) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerLifecycle::requestRespawn(playersById, id, Clock::now());
}

PlayerID
PlayerManager::onPlayerConnect(std::shared_ptr<ConnectionHandle> conn, const glm::vec3 &spawnPos) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const PlayerID id = addPlayerInternal();
    PlayerLifecycle::onPlayerConnect(
        playersById, playersOrder, id, std::move(conn), spawnPos, Clock::now()
    );
    return id;
}

bool PlayerManager::removePlayer(PlayerID id) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerLifecycle::removePlayer(playersById, playersOrder, id);
}

bool PlayerManager::touchHeartbeat(PlayerID id) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerLifecycle::touchHeartbeat(playersById, id, Clock::now());
}

bool PlayerManager::enqueuePlayerInput(PlayerID id, const PlayerInput &input) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    ServerPlayer &player = it->second;
    player.inputBuffer.enqueue(input);
    const auto now = Clock::now();
    player.lastHeartbeat = now;
    player.lastInputReceived = now;
    return true;
}

bool PlayerManager::setFlyModeAllowed(PlayerID id, bool allowed) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    ServerPlayer &player = it->second;
    player.allowFlyMode = allowed;
    if (!allowed) {
        player.flyMode = false;
        player.activeInputFlags &=
            static_cast<uint8_t>(~(kPlayerInputFlagFlyUp | kPlayerInputFlagFlyDown));
    }
    ++player.movementRevision;
    return true;
}

bool PlayerManager::setEquippedWeapon(PlayerID id, uint16_t weaponId) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerInventory::setEquippedWeapon(playersById, id, weaponId);
}

bool PlayerManager::tryFireGrapple(
    PlayerID id,
    const glm::vec3 &origin,
    const glm::vec3 &direction,
    double nowSeconds,
    const ChunkManager &chunkManager,
    GrappleFireResult &outResult
) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    ServerPlayer &player = it->second;
    if (!player.isAlive) {
        outResult = GrappleFireResult{};
        return true;
    }
    if (!inventoryHasItem(player.inventory, static_cast<uint16_t>(ITEM_GRAPPLE_GUN))) {
        outResult = GrappleFireResult{};
        return true;
    }

    GrappleGun grappleGun;
    GrappleContext context{
        .state = player.grappleState,
        .origin = origin,
        .direction = direction,
        .playerPosition = player.position,
        .nowSeconds = nowSeconds,
        .chunkManager = chunkManager
    };
    outResult = grappleGun.tryFire(context);
    ++player.movementRevision;
    return true;
}

bool PlayerManager::releaseGrapple(PlayerID id) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    GrappleGun grappleGun;
    grappleGun.release(it->second.grappleState);
    ++it->second.movementRevision;
    return true;
}

bool PlayerManager::setGrappleReeling(PlayerID id, bool reelingIn, double nowSeconds) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }
    ServerPlayer &player = it->second;
    if (!player.grappleState.active) {
        player.grappleState.reelingIn = false;
        ++player.movementRevision;
        return true;
    }

    player.grappleState.reelingIn = reelingIn;
    if (reelingIn) {
        player.grappleState.lastReelCommandTime = nowSeconds;
    }
    ++player.movementRevision;
    return true;
}

void PlayerManager::update(double deltaSeconds, ChunkManager &chunkManager) {
    const auto updateStart = std::chrono::steady_clock::now();
    size_t playerCountForLog = 0;
    const auto now = Clock::now();
    struct SimTask {
        PlayerID id = 0;
        ServerPlayer workingPlayer{};
        uint64_t baseMovementRevision = 0;
        bool hasPreparedInput = false;
        PlayerInput preparedInput{};
        uint32_t preparedInputTick = 0;
    };
    std::vector<SimTask> simTasks;
    std::vector<PlayerID> playerOrderSnapshot;
    {
        auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
        playerCountForLog = playersById.size();
        simTasks.reserve(playersById.size());
        playerOrderSnapshot.reserve(playersById.size());
        for (const auto &[id, _] : playersById) {
            playerOrderSnapshot.push_back(id);
        }

        for (const PlayerID id : playerOrderSnapshot) {
            auto it = playersById.find(id);
            if (it == playersById.end()) {
                continue;
            }

            ServerPlayer &player = it->second;
            if (!player.isAlive) {
                PlayerUpdate::handleRespawn(player, playersById, chunkManager, now);
                continue;
            }

            SimTask task{};
            task.id = id;
            task.workingPlayer = player;
            task.baseMovementRevision = player.movementRevision;
            task.hasPreparedInput =
                player.inputBuffer.peekNext(task.preparedInput, task.preparedInputTick);
            simTasks.push_back(std::move(task));
        }
    }

    for (SimTask &task : simTasks) {
        const PlayerInput *preparedInput = task.hasPreparedInput ? &task.preparedInput : nullptr;
        ServerMovementSimulation::simulatePhysicsForPlayerPrepared(
            task.workingPlayer, deltaSeconds, chunkManager, preparedInput
        );
    }

    {
        auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, "PlayerManager::update.apply");
        for (const SimTask &task : simTasks) {
            auto it = playersById.find(task.id);
            if (it == playersById.end() || !it->second.isAlive) {
                continue;
            }
            if (it->second.movementRevision != task.baseMovementRevision) {
                continue;
            }

            ServerPlayer &player = it->second;
            player.position = task.workingPlayer.position;
            player.velocity = task.workingPlayer.velocity;
            player.yaw = task.workingPlayer.yaw;
            player.pitch = task.workingPlayer.pitch;
            player.onGround = task.workingPlayer.onGround;
            player.flyMode = task.workingPlayer.flyMode;
            player.activeInputFlags = task.workingPlayer.activeInputFlags;
            player.moveX = task.workingPlayer.moveX;
            player.moveZ = task.workingPlayer.moveZ;
            player.jumpPressedLastTick = task.workingPlayer.jumpPressedLastTick;
            player.timeSinceGrounded = task.workingPlayer.timeSinceGrounded;
            player.jumpBufferTimer = task.workingPlayer.jumpBufferTimer;
            player.stepCooldownTimer = task.workingPlayer.stepCooldownTimer;
            player.grappleState = task.workingPlayer.grappleState;
            ++player.movementRevision;
            if (task.hasPreparedInput) {
                player.inputBuffer.markProcessedUpTo(task.preparedInputTick);
            }
        }

        std::vector<PlayerID> timedOutPlayerIds;
        timedOutPlayerIds.reserve(playersById.size());
        for (const auto &[id, player] : playersById) {
            if (now - player.lastHeartbeat > heartbeatTimeout) {
                timedOutPlayerIds.push_back(id);
            }
        }
        PlayerUpdate::handleTimeouts(playersById, playersOrder, timedOutPlayerIds);
    }

    const int64_t updateUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - updateStart
    )
                                 .count();
    if (g_enablePlayerManagerPerfDiagnostics.load(std::memory_order_acquire) &&
        updateUs >= kSlowPlayerManagerUpdateUs) {
        const uint64_t count =
            g_playerManagerSlowUpdateCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 40 || (count % 200) == 0) {
            std::cerr << "[perf/player-manager] slow update us=" << updateUs
                      << " players=" << playerCountForLog << " count=" << count << "\n";
        }
    }
}

std::vector<uint8_t> PlayerManager::buildSnapshotFor(PlayerID recipientId, uint32_t serverTick) {
    const std::vector<PlayerID> recipients{recipientId};
    std::vector<std::vector<uint8_t>> snapshots =
        buildSnapshotsForRecipients(recipients, serverTick);
    if (snapshots.empty()) {
        return {};
    }
    return std::move(snapshots.front());
}

std::vector<std::vector<uint8_t>> PlayerManager::buildSnapshotsForRecipients(
    const std::vector<PlayerID> &recipientIds, uint32_t serverTick
) {
    std::shared_ptr<const ReplicationSnapshot> snapshot;
    {
        auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
        snapshot = m_replicationSnapshot;
    }

    return PlayerSnapshots::buildSnapshotsForRecipients(recipientIds, serverTick, *snapshot);
}

std::vector<std::vector<uint8_t>> PlayerManager::buildSnapshotsForRecipients(
    const std::vector<std::pair<PlayerID, uint16_t>> &recipients, uint32_t serverTick
) {
    std::shared_ptr<const ReplicationSnapshot> snapshot;
    {
        auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
        snapshot = m_replicationSnapshot;
    }

    return PlayerSnapshots::buildSnapshotsForRecipients(recipients, serverTick, *snapshot);
}

void PlayerManager::CaptureReplicationSnapshot(uint32_t serverTick) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    auto snapshot = std::make_shared<ReplicationSnapshot>();
    snapshot->reserve(playersById.size());
    for (const auto &[id, player] : playersById) {
        ReplicationPlayerState state{};
        state.id = id;
        state.position = player.position;
        state.velocity = player.velocity;
        state.yaw = player.yaw;
        state.pitch = player.pitch;
        state.onGround = player.onGround;
        state.flyMode = player.flyMode;
        state.allowFlyMode = player.allowFlyMode;
        state.weaponId = player.equippedWeaponId;
        state.health = player.health;
        state.isAlive = player.isAlive;
        state.respawnAt = player.respawnAt;
        state.jumpPressedLastTick = player.jumpPressedLastTick;
        state.timeSinceGrounded = player.timeSinceGrounded;
        state.jumpBufferTimer = player.jumpBufferTimer;
        state.stepCooldownTimer = player.stepCooldownTimer;
        state.lastProcessedInputTick = player.inputBuffer.lastProcessedInputTick();
        snapshot->emplace(id, state);
    }
    m_replicationSnapshot = std::move(snapshot);
    m_replicationSnapshotTick = serverTick;
}

void PlayerManager::sendBytes(
    const std::shared_ptr<ConnectionHandle> &conn, const std::vector<uint8_t> &buf
) {
    if (!conn) {
        return;
    }
    (void)conn;
    (void)buf;
}

void PlayerManager::broadcastSnapshots() {
    std::vector<std::pair<PlayerID, std::shared_ptr<ConnectionHandle>>> recipients;
    {
        auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
        recipients.reserve(playersById.size());
        for (const auto &entry : playersById) {
            recipients.emplace_back(entry.first, entry.second.conn);
        }
    }

    for (const auto &[id, conn] : recipients) {
        std::vector<uint8_t> buf = buildSnapshotFor(id, 0);
        if (!buf.empty() && conn) {
            sendBytes(conn, buf);
        }
    }
}

std::optional<ServerPlayer> PlayerManager::getPlayerCopy(PlayerID id) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<ServerPlayer> PlayerManager::getAllPlayersCopy() {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    std::vector<ServerPlayer> players;
    players.reserve(playersById.size());
    for (const auto &entry : playersById) {
        players.push_back(entry.second);
    }
    return players;
}

std::vector<ServerPlayerCombatSnapshot> PlayerManager::getAllCombatSnapshotsCopy(bool aliveOnly) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerCombat::getAllCombatSnapshotsCopy(playersById, aliveOnly);
}

bool PlayerManager::applyDamage(PlayerID id, float damage, float &outHealthAfter, bool &outKilled) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerCombat::applyDamage(
        playersById, id, damage, outHealthAfter, outKilled, respawnDelay, Clock::now()
    );
}

bool PlayerManager::applyInventoryAction(
    PlayerID id,
    const InventoryActionRequest &request,
    InventoryActionResult &outResult,
    InventorySnapshot &outSnapshot
) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerInventory::applyInventoryAction(playersById, id, request, outResult, outSnapshot);
}

bool PlayerManager::getInventorySnapshot(PlayerID id, InventorySnapshot &outSnapshot) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerInventory::getInventorySnapshot(playersById, id, outSnapshot);
}

bool PlayerManager::getInventorySlot(PlayerID id, uint16_t slotIndex, Slot &outSlot) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerInventory::getInventorySlot(playersById, id, slotIndex, outSlot);
}

bool PlayerManager::appendItemsToInventory(
    PlayerID id,
    uint16_t itemId,
    uint16_t quantity,
    uint16_t &outAcceptedQuantity,
    InventorySnapshot *outSnapshot
) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerInventory::appendItemsToInventory(
        playersById, id, itemId, quantity, outAcceptedQuantity, outSnapshot
    );
}

bool PlayerManager::consumeItemsFromInventory(
    PlayerID id,
    const std::vector<std::pair<uint16_t, uint16_t>> &itemQuantities,
    InventorySnapshot *outSnapshot
) {
    auto lock = LockWaitTelemetry::AcquirePlayerManagerLock(mtx, __func__);
    return PlayerInventory::consumeItemsFromInventory(playersById, id, itemQuantities, outSnapshot);
}
