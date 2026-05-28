#include "BlockEditRequestService.hpp"

#include "../core/LockWaitTelemetry.hpp"
#include "../protocol/PacketParsers.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <iostream>
#include <utility>
#include <vector>

namespace {

void SendBlockPlaceResult(HSteamNetConnection incoming, const BlockPlaceResult &result) {
    const std::vector<uint8_t> bytes = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming,
        bytes.data(),
        static_cast<uint32_t>(bytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

void SendBlockBreakResult(HSteamNetConnection incoming, const BlockBreakResult &result) {
    const std::vector<uint8_t> bytes = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming,
        bytes.data(),
        static_cast<uint32_t>(bytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

} // namespace

BlockEditRequestService::BlockEditRequestService(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    WorldItemService &worldItemService,
    Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_worldItemService(worldItemService)
    , m_hooks(std::move(hooks)) {}

bool BlockEditRequestService::TryGetRegisteredPlayerId(
    HSteamNetConnection incoming, PlayerID &outPlayerId
) {
    outPlayerId = 0;
    auto lk = LockWaitTelemetry::AcquireSessionLock(
        m_mutex, "BlockEditRequestService::TryGetRegisteredPlayerId"
    );
    const auto it = m_sessions.find(incoming);
    if (it == m_sessions.end() || it->second.username.empty() || it->second.playerId == 0) {
        return false;
    }
    outPlayerId = it->second.playerId;
    return true;
}

void BlockEditRequestService::HandleBlockPlaceRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    BlockPlaceRequest request{};
    if (!NetPacket::ParseBlockPlaceRequestPacket(
            reinterpret_cast<const uint8_t *>(data), size, request
        )) {
        BlockPlaceResult result{};
        result.accepted = 0;
        result.rejectReason = BlockPlaceRejectReason::InvalidPacket;
        SendBlockPlaceResult(incoming, result);
        std::cerr << "[block/place] malformed request size=" << size << "\n";
        return;
    }

    PlayerID requesterId = 0;
    if (!TryGetRegisteredPlayerId(incoming, requesterId)) {
        BlockPlaceResult result{};
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = BlockPlaceRejectReason::Unregistered;
        SendBlockPlaceResult(incoming, result);
        return;
    }
    m_playerManager.touchHeartbeat(requesterId);

    const BlockPlaceResult result = m_hooks.executeBlockPlaceRequest(requesterId, request);
    SendBlockPlaceResult(incoming, result);
    if (result.accepted != 0) {
        m_worldItemService.SendInventorySnapshotToPlayer(requesterId);
    }
}

void BlockEditRequestService::HandleBlockBreakRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    BlockBreakRequest request{};
    if (!NetPacket::ParseBlockBreakRequestPacket(
            reinterpret_cast<const uint8_t *>(data), size, request
        )) {
        BlockBreakResult result{};
        result.accepted = 0;
        result.rejectReason = BlockBreakRejectReason::InvalidPacket;
        SendBlockBreakResult(incoming, result);
        std::cerr << "[block/break] malformed request size=" << size << "\n";
        return;
    }

    PlayerID requesterId = 0;
    if (!TryGetRegisteredPlayerId(incoming, requesterId)) {
        BlockBreakResult result{};
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = BlockBreakRejectReason::Unregistered;
        SendBlockBreakResult(incoming, result);
        return;
    }
    m_playerManager.touchHeartbeat(requesterId);

    const BlockBreakResult result = m_hooks.executeBlockBreakRequest(requesterId, request);
    SendBlockBreakResult(incoming, result);
}
