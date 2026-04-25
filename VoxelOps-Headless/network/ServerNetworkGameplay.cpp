#include "ServerNetwork.hpp"
#include "PacketParsers.hpp"

#include <glm/trigonometric.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
constexpr uint32_t kServerTickRateHz = 60u;
}

void ServerNetwork::SpawnDroppedItem(PlayerID dropperId, uint16_t itemId, uint16_t quantity) {
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
    item.pickupCooldownSeconds = WorldItemPhysicsSystem::kPickupCooldownSeconds;
    item.ttlSeconds = WorldItemPhysicsSystem::kTtlSeconds;
    m_worldItems[item.id] = item;
}

void ServerNetwork::SendInventorySnapshotToPlayer(PlayerID playerId) {
    HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto &[sessionConn, session] : m_clients) {
            if (session.playerId == playerId) {
                conn = sessionConn;
                break;
            }
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
        conn, snapshotBytes.data(), static_cast<uint32_t>(snapshotBytes.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);
}

void ServerNetwork::UpdateWorldItems(double deltaSeconds) {
    if (deltaSeconds <= 0.0 || m_worldItems.empty()) {
        return;
    }

    const float dt = static_cast<float>(deltaSeconds);
    const float pickupRadiusSq =
        WorldItemPhysicsSystem::kPickupRadius * WorldItemPhysicsSystem::kPickupRadius;
    std::unordered_set<PlayerID> inventoryChangedPlayers;

    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();

    for (auto it = m_worldItems.begin(); it != m_worldItems.end();) {
        WorldItemEntity &item = it->second;
        item.ttlSeconds = std::max(0.0f, item.ttlSeconds - dt);
        item.pickupCooldownSeconds = std::max(0.0f, item.pickupCooldownSeconds - dt);
        if (item.ttlSeconds <= 0.0f || item.quantity == 0) {
            it = m_worldItems.erase(it);
            continue;
        }

        WorldItemPhysicsSystem::Step(item, dt, static_cast<float>(kServerTickRateHz),
                                     m_chunkManager);

        if (item.pickupCooldownSeconds <= 0.0f) {
            for (const ServerPlayer &player : players) {
                if (!player.isAlive) {
                    continue;
                }
                const glm::vec3 delta = player.position - item.position;
                if (glm::dot(delta, delta) > pickupRadiusSq) {
                    continue;
                }

                uint16_t acceptedQuantity = 0;
                if (m_playerManager.appendItemsToInventory(player.id, item.itemId, item.quantity,
                                                           acceptedQuantity, nullptr) &&
                    acceptedQuantity > 0) {
                    item.quantity = static_cast<uint16_t>(item.quantity - acceptedQuantity);
                    inventoryChangedPlayers.insert(player.id);
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

void ServerNetwork::SendWorldItemSnapshots(
    const std::vector<std::pair<HSteamNetConnection, PlayerID>> &recipients, uint32_t serverTick) {
    for (const auto &[conn, playerId] : recipients) {
        const std::optional<ServerPlayer> playerOpt = m_playerManager.getPlayerCopy(playerId);
        if (!playerOpt.has_value()) {
            continue;
        }

        WorldItemSnapshot snapshot{};
        snapshot.serverTick = serverTick;
        const glm::vec3 playerPos = playerOpt->position;
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
            conn, bytes.data(), static_cast<uint32_t>(bytes.size()),
            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
    }
}

void ServerNetwork::HandleInventoryActionRequestPacket(HSteamNetConnection incoming,
                                                       const void *data, uint32_t size) {
    InventoryActionRequest request{};
    if (!NetPacket::ParseInventoryActionRequestPacket(reinterpret_cast<const uint8_t *>(data), size,
                                                      request)) {
        std::cerr << "[recv] malformed InventoryActionRequest\n";
        return;
    }

    PlayerID playerId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            playerId = it->second.playerId;
        }
    }
    if (playerId == 0) {
        return;
    }

    m_playerManager.touchHeartbeat(playerId);

    Slot preDropSlot{};
    bool hasPreDropSlot = false;
    if (request.action.type == InventoryActionType::Drop) {
        hasPreDropSlot =
            m_playerManager.getInventorySlot(playerId, request.action.sourceSlot, preDropSlot);
    }

    InventoryActionResult result{};
    InventorySnapshot snapshot{};
    if (!m_playerManager.applyInventoryAction(playerId, request, result, snapshot)) {
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = InventoryRejectReason::Unsupported;
        result.changedSlots.clear();
        result.newRevision = 0;
        const std::vector<uint8_t> resultBytes = result.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming, resultBytes.data(), static_cast<uint32_t>(resultBytes.size()),
            k_nSteamNetworkingSend_Reliable, nullptr);
        return;
    }

    const std::vector<uint8_t> resultBytes = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming, resultBytes.data(), static_cast<uint32_t>(resultBytes.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);

    const std::vector<uint8_t> snapshotBytes = snapshot.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming, snapshotBytes.data(), static_cast<uint32_t>(snapshotBytes.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);

    if (result.accepted != 0 && request.action.type == InventoryActionType::Drop &&
        hasPreDropSlot && !Inventory::IsEmpty(preDropSlot) &&
        Inventory::IsValidItemId(preDropSlot.itemId)) {
        const uint16_t requestedAmount = (request.action.amount == 0)
                                             ? preDropSlot.quantity
                                             : static_cast<uint16_t>(std::min<uint16_t>(
                                                   request.action.amount, preDropSlot.quantity));
        if (requestedAmount > 0) {
            SpawnDroppedItem(playerId, preDropSlot.itemId, requestedAmount);
        }
    }
}
