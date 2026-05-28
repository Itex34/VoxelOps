#include "WorldItemService.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr uint32_t kServerTickRateHz = 60u;
} // namespace

WorldItemService::WorldItemService(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    ChunkManager &chunkManager
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_chunkManager(chunkManager) {}

void WorldItemService::Reset() {
    m_worldItems.clear();
    m_nextWorldItemId = 1;
}

void WorldItemService::SpawnDroppedItem(PlayerID dropperId, uint16_t itemId, uint16_t quantity) {
    if (!Inventory::IsValidItemId(itemId) || quantity == 0) {
        return;
    }

    const std::optional<ServerPlayer> playerOpt = m_playerManager.getPlayerCopy(dropperId);
    if (!playerOpt.has_value()) {
        return;
    }

    const ServerPlayer &player = *playerOpt;
    const float yawRad = glm::radians(player.yaw);
    const glm::vec3 forward(std::cos(yawRad), 0.0f, std::sin(yawRad));

    WorldItemEntity item{};
    item.id = m_nextWorldItemId++;
    item.itemId = itemId;
    item.quantity = quantity;
    item.position = player.position + glm::vec3(0.0f, 1.25f, 0.0f) + (forward * 0.65f);
    item.velocity = forward * 3.0f + glm::vec3(0.0f, 3.2f, 0.0f);
    item.pickupCooldownSeconds = WorldItemPhysics::kPickupCooldownSeconds;
    item.ttlSeconds = WorldItemPhysics::kTtlSeconds;
    m_worldItems[item.id] = item;
}

void WorldItemService::SendInventorySnapshotToPlayer(PlayerID playerId) {
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "WorldItemService::SendInventorySnapshotToPlayer"
        );
        const auto connOpt = m_sessions.FindConnectionByPlayerId(playerId);
        if (connOpt.has_value()) {
            conn = *connOpt;
        }
    }
    if (conn == k_HSteamNetConnection_Invalid) {
        return;
    }

    InventorySnapshot snapshot{};
    if (!m_playerManager.getInventorySnapshot(playerId, snapshot)) {
        return;
    }
    const std::vector<uint8_t> snapshotBytes = snapshot.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        conn,
        snapshotBytes.data(),
        static_cast<uint32_t>(snapshotBytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

void WorldItemService::UpdateWorldItems(double deltaSeconds) {
    if (deltaSeconds <= 0.0 || m_worldItems.empty()) {
        return;
    }

    const float dt = static_cast<float>(deltaSeconds);
    const float pickupRadiusSq = WorldItemPhysics::kPickupRadius * WorldItemPhysics::kPickupRadius;
    std::unordered_set<PlayerID> inventoryChangedPlayers;

    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    std::vector<const ServerPlayer *> alivePlayers;
    alivePlayers.reserve(players.size());
    for (const ServerPlayer &player : players) {
        if (player.isAlive) {
            alivePlayers.push_back(&player);
        }
    }

    for (auto it = m_worldItems.begin(); it != m_worldItems.end();) {
        WorldItemEntity &item = it->second;
        item.ttlSeconds = std::max(0.0f, item.ttlSeconds - dt);
        item.pickupCooldownSeconds = std::max(0.0f, item.pickupCooldownSeconds - dt);
        if (item.ttlSeconds <= 0.0f || item.quantity == 0) {
            it = m_worldItems.erase(it);
            continue;
        }

        WorldItemPhysics::Step(item, dt, static_cast<float>(kServerTickRateHz), m_chunkManager);

        if (item.pickupCooldownSeconds <= 0.0f) {
            for (const ServerPlayer *player : alivePlayers) {
                const glm::vec3 delta = player->position - item.position;
                if (glm::dot(delta, delta) > pickupRadiusSq) {
                    continue;
                }

                uint16_t acceptedQuantity = 0;
                if (m_playerManager.appendItemsToInventory(
                        player->id, item.itemId, item.quantity, acceptedQuantity, nullptr
                    ) &&
                    acceptedQuantity > 0) {
                    item.quantity = static_cast<uint16_t>(item.quantity - acceptedQuantity);
                    inventoryChangedPlayers.insert(player->id);
                    if (item.quantity == 0) {
                        break;
                    }
                }
            }
        }

        if (item.quantity == 0) {
            it = m_worldItems.erase(it);
            continue;
        }

        ++it;
    }

    for (const PlayerID playerId : inventoryChangedPlayers) {
        SendInventorySnapshotToPlayer(playerId);
    }
}

void WorldItemService::SendWorldItemSnapshots(
    const std::vector<std::pair<HSteamNetConnection, PlayerID>> &recipients, uint32_t serverTick
) {
    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    std::unordered_map<PlayerID, glm::vec3> playerPositions;
    playerPositions.reserve(players.size());
    for (const ServerPlayer &player : players) {
        playerPositions.emplace(player.id, player.position);
    }

    for (const auto &[conn, playerId] : recipients) {
        const auto playerIt = playerPositions.find(playerId);
        if (playerIt == playerPositions.end()) {
            continue;
        }

        WorldItemSnapshot snapshot{};
        snapshot.serverTick = serverTick;
        const glm::vec3 &playerPos = playerIt->second;
        constexpr float kItemReplicateRadius = 40.0f;
        const float radiusSq = kItemReplicateRadius * kItemReplicateRadius;

        snapshot.items.reserve(m_worldItems.size());
        for (const auto &[_, item] : m_worldItems) {
            const glm::vec3 delta = item.position - playerPos;
            if (glm::dot(delta, delta) > radiusSq) {
                continue;
            }
            WorldItemState state{};
            state.id = item.id;
            state.itemId = item.itemId;
            state.quantity = item.quantity;
            state.px = item.position.x;
            state.py = item.position.y;
            state.pz = item.position.z;
            state.vx = item.velocity.x;
            state.vy = item.velocity.y;
            state.vz = item.velocity.z;
            snapshot.items.push_back(state);
        }

        const std::vector<uint8_t> bytes = snapshot.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            conn,
            bytes.data(),
            static_cast<uint32_t>(bytes.size()),
            k_nSteamNetworkingSend_UnreliableNoDelay,
            nullptr
        );
    }
}
