#include "InventoryActionService.hpp"

#include "../core/LockWaitTelemetry.hpp"
#include "../protocol/PacketParsers.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <algorithm>
#include <iostream>
#include <vector>

InventoryActionService::InventoryActionService(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    WorldItemService &worldItemService
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_worldItemService(worldItemService) {}

void InventoryActionService::HandleInventoryActionRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    InventoryActionRequest request{};
    if (!NetPacket::ParseInventoryActionRequestPacket(
            reinterpret_cast<const uint8_t *>(data), size, request
        )) {
        std::cerr << "[recv] malformed InventoryActionRequest\n";
        return;
    }

    PlayerID playerId = 0;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "InventoryActionService::HandleInventoryActionRequestPacket"
        );
        auto it = m_sessions.find(incoming);
        if (it != m_sessions.end()) {
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
            incoming,
            resultBytes.data(),
            static_cast<uint32_t>(resultBytes.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
        return;
    }

    const std::vector<uint8_t> resultBytes = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming,
        resultBytes.data(),
        static_cast<uint32_t>(resultBytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );

    const std::vector<uint8_t> snapshotBytes = snapshot.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming,
        snapshotBytes.data(),
        static_cast<uint32_t>(snapshotBytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );

    if (result.accepted != 0 && request.action.type == InventoryActionType::Drop &&
        hasPreDropSlot && !Inventory::IsEmpty(preDropSlot) &&
        Inventory::IsValidItemId(preDropSlot.itemId)) {
        const uint16_t requestedAmount = (request.action.amount == 0)
                                             ? preDropSlot.quantity
                                             : static_cast<uint16_t>(std::min<uint16_t>(
                                                   request.action.amount, preDropSlot.quantity
                                               ));
        if (requestedAmount > 0) {
            m_worldItemService.SpawnDroppedItem(playerId, preDropSlot.itemId, requestedAmount);
        }
    }
}
