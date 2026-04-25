#include "PlayerLifecycle.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/PlayerData.hpp"

#include "../gameplay/spawn/Respawning.hpp"

#include <iostream>
#include <iterator>
#include <utility>

namespace {
inline const Shared::PlayerData::MovementSettings &movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}
} // namespace

namespace PlayerLifecycle {

void onPlayerConnect(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                     std::list<PlayerID> &playersOrder,
                     PlayerID id,
                     std::shared_ptr<ConnectionHandle> conn,
                     const glm::vec3 &spawnPos,
                     Clock::time_point now) {
    ServerPlayer player;
    player.id = id;
    player.position = spawnPos;
    player.velocity = glm::vec3(0.0f);
    player.height = movementSettings().collisionHeight;
    player.radius = movementSettings().collisionRadius;
    player.health = player.maxHealth;
    player.isAlive = true;
    player.respawnAt = Clock::time_point{};
    player.pendingRespawnRequest = false;
    player.lastHeartbeat = now;
    player.lastInputReceived = now;
    player.conn = std::move(conn);

    (void)player.inventory.appendItems(static_cast<uint16_t>(ITEM_PISTOL), 1);
    (void)player.inventory.appendItems(static_cast<uint16_t>(ITEM_SNIPER), 1);
    (void)player.inventory.appendItems(static_cast<uint16_t>(ITEM_PISTOL_AMMO), 48);
    (void)player.inventory.appendItems(static_cast<uint16_t>(ITEM_SAPPHIRE_BLOCK), kMaxBlockStack);
    (void)player.inventory.appendItems(static_cast<uint16_t>(ITEM_RUBY_BLOCK), kMaxBlockStack);

    playersOrder.push_back(id);
    player.orderIt = std::prev(playersOrder.end());

    playersById.emplace(id, std::move(player));
    std::cout << "Player " << id << " connected\n";
}

bool removePlayer(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                  std::list<PlayerID> &playersOrder,
                  PlayerID id) {
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    playersOrder.erase(it->second.orderIt);
    playersById.erase(it);
    std::cout << "Player " << id << " removed\n";
    return true;
}

bool touchHeartbeat(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                    PlayerID id,
                    Clock::time_point now) {
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }
    it->second.lastHeartbeat = now;
    return true;
}

bool requestRespawn(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                    PlayerID id,
                    Clock::time_point now) {
    const auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    return Respawning::RequestRespawn(it->second, now);
}

} // namespace PlayerLifecycle
