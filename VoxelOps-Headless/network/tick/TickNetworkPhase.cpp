#include "TickNetworkPhase.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <iostream>
#include <utility>

TickNetworkPhase::TickNetworkPhase(
    std::mutex &mutex, ClientSessionManager &sessions, HSteamNetPollGroup &pollGroup, Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_hooks(std::move(hooks))
    , m_networkPump(
          pollGroup,
          NetworkPump::Hooks{
              m_hooks.isInboundRateLimitExceeded,
              m_hooks.onConnectRequest,
              m_hooks.onMessage,
              m_hooks.onPlayerInput,
              m_hooks.onChunkRequest,
              m_hooks.onBlockPlaceRequest,
              m_hooks.onBlockBreakRequest,
              m_hooks.onShootRequest,
              m_hooks.onGrappleRequest,
              m_hooks.onInventoryActionRequest,
          }
      ) {}

void TickNetworkPhase::RunInboundMessagePhase(
    uint64_t &msgPacketsThisLoop,
    uint64_t &playerInputPacketsThisLoop,
    uint64_t &chunkRequestPacketsThisLoop,
    double &messageDrainUs
) {
    m_networkPump.PumpInbound(
        msgPacketsThisLoop, playerInputPacketsThisLoop, chunkRequestPacketsThisLoop, messageDrainUs
    );
}

void TickNetworkPhase::RunConnectionCleanupPhase() {
    std::vector<std::pair<HSteamNetConnection, ClientSession>> sessionSnapshot;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "TickNetworkPhase::RunConnectionCleanupPhase.snapshot"
        );
        sessionSnapshot.reserve(m_sessions.size());
        for (const auto &[conn, session] : m_sessions) {
            sessionSnapshot.emplace_back(conn, session);
        }
    }

    std::vector<std::pair<HSteamNetConnection, ClientSession>> staleConnections;
    staleConnections.reserve(sessionSnapshot.size());
    for (const auto &[conn, session] : sessionSnapshot) {
        SteamNetConnectionInfo_t info{};
        const bool haveInfo = SteamNetworkingSockets()->GetConnectionInfo(conn, &info);
        if (!haveInfo || info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            staleConnections.emplace_back(conn, session);
        }
    }

    if (!staleConnections.empty()) {
        std::vector<std::pair<HSteamNetConnection, ClientSession>> removedSessions;
        removedSessions.reserve(staleConnections.size());
        {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_mutex, "TickNetworkPhase::RunConnectionCleanupPhase.remove"
            );
            for (const auto &[conn, _] : staleConnections) {
                ClientSession removed{};
                if (m_sessions.RemoveConnection(conn, &removed)) {
                    removedSessions.emplace_back(conn, std::move(removed));
                }
            }
        }
        staleConnections = std::move(removedSessions);
    } else {
        staleConnections.clear();
    }

    for (const auto &[conn, session] : staleConnections) {
        std::cout << "[cleanup] remove conn=" << conn << " user=" << session.username << "\n";
        m_hooks.teardownClientSession(conn, session, "server cleanup", true);
    }
}
