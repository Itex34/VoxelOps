#include "NetworkPump.hpp"

#include "../protocol/PacketValidation.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <chrono>
#include <iostream>
#include <utility>

namespace {

    constexpr size_t kMaxInboundMessagesPerLoop = 64;
    constexpr size_t kMaxConnectRequestsPerLoop = 2;
    constexpr size_t kMaxDeferredInboundPackets = 512;
    constexpr int64_t kInboundMessageBudgetUs = 1500;

} // namespace

NetworkPump::NetworkPump(HSteamNetPollGroup &pollGroup, Hooks hooks)
    : m_pollGroup(pollGroup)
    , m_hooks(std::move(hooks)) {}

void NetworkPump::DispatchInboundPacket(
    HSteamNetConnection incoming,
    PacketType packetType,
    const void *data,
    uint32_t size,
    uint64_t &playerInputPacketsThisLoop,
    uint64_t &chunkRequestPacketsThisLoop
) {
    switch (packetType) {
    case PacketType::ConnectRequest:
        m_hooks.onConnectRequest(incoming, data, size);
        return;
    case PacketType::Message:
        m_hooks.onMessage(incoming, data, size);
        return;
    case PacketType::PlayerInput:
        m_hooks.onPlayerInput(incoming, data, size, playerInputPacketsThisLoop);
        return;
    case PacketType::ChunkRequest:
        m_hooks.onChunkRequest(incoming, data, size, chunkRequestPacketsThisLoop);
        return;
    case PacketType::BlockPlaceRequest:
        m_hooks.onBlockPlaceRequest(incoming, data, size);
        return;
    case PacketType::BlockBreakRequest:
        m_hooks.onBlockBreakRequest(incoming, data, size);
        return;
    case PacketType::ShootRequest:
        m_hooks.onShootRequest(incoming, data, size);
        return;
    case PacketType::GrappleRequest:
        m_hooks.onGrappleRequest(incoming, data, size);
        return;
    case PacketType::InventoryActionRequest:
        m_hooks.onInventoryActionRequest(incoming, data, size);
        return;
    default:
        return;
    }
}

void NetworkPump::PumpInbound(
    uint64_t &msgPacketsThisLoop,
    uint64_t &playerInputPacketsThisLoop,
    uint64_t &chunkRequestPacketsThisLoop,
    double &messageDrainUs
) {
    SteamNetworkingSockets()->RunCallbacks();

    const auto messageDrainStart = std::chrono::steady_clock::now();
    SteamNetworkingMessage_t *pMsg = nullptr;
    size_t inboundMessagesProcessed = 0;
    size_t connectRequestsProcessed = 0;
    const auto reachedMessageDrainBudget = [&]() {
        ++inboundMessagesProcessed;
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - messageDrainStart
        )
                                      .count();
        return elapsedUs >= kInboundMessageBudgetUs;
    };

    while (inboundMessagesProcessed < kMaxInboundMessagesPerLoop && !m_deferredPackets.empty()) {
        DeferredPacket packet = std::move(m_deferredPackets.front());
        if (packet.packetType == PacketType::ConnectRequest &&
            connectRequestsProcessed >= kMaxConnectRequestsPerLoop) {
            m_deferredPackets.front() = std::move(packet);
            break;
        }
        m_deferredPackets.pop_front();
        if (packet.packetType == PacketType::ConnectRequest) {
            ++connectRequestsProcessed;
        }
        ++msgPacketsThisLoop;
        DispatchInboundPacket(
            packet.conn,
            packet.packetType,
            packet.bytes.data(),
            static_cast<uint32_t>(packet.bytes.size()),
            playerInputPacketsThisLoop,
            chunkRequestPacketsThisLoop
        );
        if (reachedMessageDrainBudget()) {
            break;
        }
    }

    while (inboundMessagesProcessed < kMaxInboundMessagesPerLoop &&
           SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, &pMsg, 1) > 0 &&
           pMsg) {
        const HSteamNetConnection incoming = pMsg->m_conn;
        const void *data = pMsg->m_pData;
        const uint32_t cb = pMsg->m_cbSize;

        if (cb < 1 || cb > NetPacket::kMaxInboundPacketBytes) {
            std::cerr << "[recv] invalid packet size=" << cb << " conn=" << incoming
                      << " (closing connection)\n";
            SteamNetworkingSockets()->CloseConnection(incoming, 0, "invalid packet size", false);
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        const PacketType packetType =
            static_cast<PacketType>(reinterpret_cast<const uint8_t *>(data)[0]);
        if (!NetPacket::IsInboundPacketSizeValid(packetType, cb)) {
            std::cerr << "[recv] packet size/type mismatch type=" << static_cast<int>(packetType)
                      << " size=" << cb << " conn=" << incoming << "\n";
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        if (m_hooks.isInboundRateLimitExceeded(incoming, packetType, cb)) {
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        if (packetType == PacketType::ConnectRequest &&
            connectRequestsProcessed >= kMaxConnectRequestsPerLoop) {
            if (m_deferredPackets.size() < kMaxDeferredInboundPackets) {
                DeferredPacket deferred;
                deferred.conn = incoming;
                deferred.packetType = packetType;
                const auto *begin = reinterpret_cast<const uint8_t *>(data);
                deferred.bytes.assign(begin, begin + cb);
                m_deferredPackets.push_back(std::move(deferred));
            } else {
                std::cerr << "[recv] deferred packet queue full conn=" << incoming
                          << " type=" << static_cast<int>(packetType)
                          << " (closing connection)\n";
                SteamNetworkingSockets()->CloseConnection(
                    incoming, 0, "server inbound queue full", false
                );
            }
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        if (packetType == PacketType::ConnectRequest) {
            ++connectRequestsProcessed;
        }
        ++msgPacketsThisLoop;
        DispatchInboundPacket(
            incoming, packetType, data, cb, playerInputPacketsThisLoop, chunkRequestPacketsThisLoop
        );
        pMsg->Release();
        if (reachedMessageDrainBudget()) {
            break;
        }
    }

    messageDrainUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - messageDrainStart
    )
                                             .count());
}
