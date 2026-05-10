#include "PlayerManager.hpp"

#include "PlayerLifecycle.hpp"
#include "PlayerUpdate.hpp"
#include "inventory/PlayerInventory.hpp"

#include "../combat/PlayerCombat.hpp"
#include "../../network/snapshots/PlayerSnapshots.hpp"
#include "../player/ServerMovementSimulation.hpp"
#include "../../engine/world/ChunkManager.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <utility>

namespace {
    constexpr int64_t kSlowPlayerManagerUpdateUs = 4000;
    std::atomic<uint64_t> g_playerManagerSlowUpdateCount{0};
    std::atomic<bool> g_enablePlayerManagerPerfDiagnostics{false};
} // namespace

PlayerManager::PlayerManager() = default;

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
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerLifecycle::requestRespawn(playersById, id, Clock::now());
}

PlayerID
PlayerManager::onPlayerConnect(std::shared_ptr<ConnectionHandle> conn, const glm::vec3 &spawnPos) {
    std::lock_guard<std::mutex> lock(mtx);
    const PlayerID id = addPlayerInternal();
    PlayerLifecycle::onPlayerConnect(
        playersById, playersOrder, id, std::move(conn), spawnPos, Clock::now()
    );
    return id;
}

bool PlayerManager::removePlayer(PlayerID id) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerLifecycle::removePlayer(playersById, playersOrder, id);
}

bool PlayerManager::touchHeartbeat(PlayerID id) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerLifecycle::touchHeartbeat(playersById, id, Clock::now());
}

bool PlayerManager::enqueuePlayerInput(PlayerID id, const PlayerInput &input) {
    std::lock_guard<std::mutex> lock(mtx);
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
    std::lock_guard<std::mutex> lock(mtx);
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
    return true;
}

bool PlayerManager::setEquippedWeapon(PlayerID id, uint16_t weaponId) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerInventory::setEquippedWeapon(playersById, id, weaponId);
}

void PlayerManager::update(double deltaSeconds, ChunkManager &chunkManager) {
    const auto updateStart = std::chrono::steady_clock::now();
    size_t playerCountForLog = 0;
    {
        std::lock_guard<std::mutex> lock(mtx);
        playerCountForLog = playersById.size();
        PlayerUpdate::updatePlayers(
            playersById, playersOrder, deltaSeconds, chunkManager, heartbeatTimeout
        );
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
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerSnapshots::buildSnapshotFor(recipientId, serverTick, playersById);
}

std::vector<std::vector<uint8_t>> PlayerManager::buildSnapshotsForRecipients(
    const std::vector<PlayerID> &recipientIds, uint32_t serverTick
) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerSnapshots::buildSnapshotsForRecipients(recipientIds, serverTick, playersById);
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
        std::lock_guard<std::mutex> lock(mtx);
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
    std::lock_guard<std::mutex> lock(mtx);
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<ServerPlayer> PlayerManager::getAllPlayersCopy() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<ServerPlayer> players;
    players.reserve(playersById.size());
    for (const auto &entry : playersById) {
        players.push_back(entry.second);
    }
    return players;
}

std::vector<ServerPlayerCombatSnapshot> PlayerManager::getAllCombatSnapshotsCopy(bool aliveOnly) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerCombat::getAllCombatSnapshotsCopy(playersById, aliveOnly);
}

bool PlayerManager::applyDamage(PlayerID id, float damage, float &outHealthAfter, bool &outKilled) {
    std::lock_guard<std::mutex> lock(mtx);
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
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerInventory::applyInventoryAction(playersById, id, request, outResult, outSnapshot);
}

bool PlayerManager::getInventorySnapshot(PlayerID id, InventorySnapshot &outSnapshot) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerInventory::getInventorySnapshot(playersById, id, outSnapshot);
}

bool PlayerManager::getInventorySlot(PlayerID id, uint16_t slotIndex, Slot &outSlot) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerInventory::getInventorySlot(playersById, id, slotIndex, outSlot);
}

bool PlayerManager::appendItemsToInventory(
    PlayerID id,
    uint16_t itemId,
    uint16_t quantity,
    uint16_t &outAcceptedQuantity,
    InventorySnapshot *outSnapshot
) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerInventory::appendItemsToInventory(
        playersById, id, itemId, quantity, outAcceptedQuantity, outSnapshot
    );
}

bool PlayerManager::consumeItemsFromInventory(
    PlayerID id,
    const std::vector<std::pair<uint16_t, uint16_t>> &itemQuantities,
    InventorySnapshot *outSnapshot
) {
    std::lock_guard<std::mutex> lock(mtx);
    return PlayerInventory::consumeItemsFromInventory(playersById, id, itemQuantities, outSnapshot);
}
