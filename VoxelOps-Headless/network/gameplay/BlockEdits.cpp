#include "../core/Runtime.hpp"
#include "../protocol/PacketParsers.hpp"

#include <iostream>
#include <vector>

namespace {

void SendBlockPlaceResult(HSteamNetConnection incoming, const BlockPlaceResult &result) {
    const std::vector<uint8_t> bytes = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming, bytes.data(), static_cast<uint32_t>(bytes.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);
}

void SendBlockBreakResult(HSteamNetConnection incoming, const BlockBreakResult &result) {
    const std::vector<uint8_t> bytes = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming, bytes.data(), static_cast<uint32_t>(bytes.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);
}

} // namespace

bool Runtime::TryGetRegisteredPlayerId(HSteamNetConnection incoming, PlayerID &outPlayerId) {
    outPlayerId = 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_clients.find(incoming);
    if (it == m_clients.end() || it->second.username.empty() || it->second.playerId == 0) {
        return false;
    }
    outPlayerId = it->second.playerId;
    return true;
}

void Runtime::HandleBlockPlaceRequestPacket(HSteamNetConnection incoming, const void *data,
                                                  uint32_t size) {
    BlockPlaceRequest request{};
    if (!NetPacket::ParseBlockPlaceRequestPacket(reinterpret_cast<const uint8_t *>(data), size,
                                                 request)) {
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

    const BlockPlaceResult result = ExecuteBlockPlaceRequest(requesterId, request);
    SendBlockPlaceResult(incoming, result);
}

void Runtime::HandleBlockBreakRequestPacket(HSteamNetConnection incoming, const void *data,
                                                  uint32_t size) {
    BlockBreakRequest request{};
    if (!NetPacket::ParseBlockBreakRequestPacket(reinterpret_cast<const uint8_t *>(data), size,
                                                 request)) {
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

    const BlockBreakResult result = ExecuteBlockBreakRequest(requesterId, request);
    SendBlockBreakResult(incoming, result);
}
