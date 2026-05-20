#include "../Runtime.hpp"
#include "../../protocol/PacketValidation.hpp"

void Runtime::DispatchInboundPacket(
    HSteamNetConnection incoming,
    PacketType packetType,
    const void *data,
    uint32_t size,
    uint64_t &playerInputPacketsThisLoop,
    uint64_t &chunkRequestPacketsThisLoop
) {
    switch (packetType) {
    case PacketType::ConnectRequest:
        HandleConnectRequest(incoming, data, size);
        return;
    case PacketType::Message:
        HandleMessagePacket(incoming, data, size);
        return;
    case PacketType::PlayerInput:
        HandlePlayerInputPacket(incoming, data, size, playerInputPacketsThisLoop);
        return;
    case PacketType::ChunkRequest:
        HandleChunkRequestPacket(incoming, data, size, chunkRequestPacketsThisLoop);
        return;
    case PacketType::BlockPlaceRequest:
        HandleBlockPlaceRequestPacket(incoming, data, size);
        return;
    case PacketType::BlockBreakRequest:
        HandleBlockBreakRequestPacket(incoming, data, size);
        return;
    case PacketType::ShootRequest:
        HandleShootRequestPacket(incoming, data, size);
        return;
    case PacketType::GrappleRequest:
        HandleGrappleRequestPacket(incoming, data, size);
        return;
    case PacketType::InventoryActionRequest:
        HandleInventoryActionRequestPacket(incoming, data, size);
        return;
    default:
        return;
    }
}

void Runtime::RunInboundMessagePhase(
    uint64_t &msgPacketsThisLoop,
    uint64_t &playerInputPacketsThisLoop,
    uint64_t &chunkRequestPacketsThisLoop,
    double &messageDrainUs
) {
    constexpr size_t kMaxInboundMessagesPerLoop = 256;
    constexpr int64_t kInboundMessageBudgetUs = 3000;

    SteamNetworkingSockets()->RunCallbacks();

    // Receive messages on poll group (any connection assigned to it).
    // Drain with a bounded budget so message bursts do not starve simulation ticks.
    const auto messageDrainStart = std::chrono::steady_clock::now();
    SteamNetworkingMessage_t *pMsg = nullptr;
    size_t inboundMessagesProcessed = 0;
    const auto reachedMessageDrainBudget = [&]() {
        ++inboundMessagesProcessed;
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - messageDrainStart
        )
                                      .count();
        return elapsedUs >= kInboundMessageBudgetUs;
    };
    while (inboundMessagesProcessed < kMaxInboundMessagesPerLoop &&
           SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, &pMsg, 1) > 0 &&
           pMsg) {
        HSteamNetConnection incoming = pMsg->m_conn;
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

        if (IsInboundRateLimitExceeded(incoming, packetType, cb)) {
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
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

void Runtime::RunConnectionCleanupPhase() {
    // Optional: extra safeguard - check connection states for any connections left
    // (callback already handles most).
    std::vector<std::pair<HSteamNetConnection, ClientSession>> staleConnections;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            HSteamNetConnection conn = it->first;
            SteamNetConnectionInfo_t info;
            if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info) &&
                (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                 info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)) {
                staleConnections.emplace_back(conn, it->second);
                it = m_clients.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (const auto &[conn, session] : staleConnections) {
        std::cout << "[cleanup] remove conn=" << conn << " user=" << session.username << "\n";
        TeardownClientSession(conn, session, "server cleanup", true);
    }
}
