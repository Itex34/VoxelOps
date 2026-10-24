#include "TickNetworkPhase.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <iostream>
#include <utility>

namespace {

    const char *ConnectionStateName(ESteamNetworkingConnectionState state) {
        switch (state) {
        case k_ESteamNetworkingConnectionState_None:
            return "none";
        case k_ESteamNetworkingConnectionState_Connecting:
            return "connecting";
        case k_ESteamNetworkingConnectionState_FindingRoute:
            return "finding_route";
        case k_ESteamNetworkingConnectionState_Connected:
            return "connected";
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            return "closed_by_peer";
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            return "problem_detected_locally";
        default:
            return "unknown";
        }
    }

} // namespace

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
    m_connectionSnapshot.clear();
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "TickNetworkPhase::RunConnectionCleanupPhase.snapshot"
        );
        m_connectionSnapshot.reserve(m_sessions.size());
        for (const auto &[conn, _] : m_sessions) {
            m_connectionSnapshot.push_back(conn);
        }
    }

    struct StaleConnection {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        bool haveInfo = false;
        SteamNetConnectionInfo_t info{};
    };
    std::vector<StaleConnection> staleConnections;
    staleConnections.reserve(m_connectionSnapshot.size());
    for (const HSteamNetConnection conn : m_connectionSnapshot) {
        SteamNetConnectionInfo_t info{};
        const bool haveInfo = SteamNetworkingSockets()->GetConnectionInfo(conn, &info);
        if (!haveInfo || info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            staleConnections.push_back(StaleConnection{conn, haveInfo, info});
        }
    }

    if (!staleConnections.empty()) {
        std::vector<std::pair<StaleConnection, ClientSession>> removedSessions;
        removedSessions.reserve(staleConnections.size());
        {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_mutex, "TickNetworkPhase::RunConnectionCleanupPhase.remove"
            );
            for (const StaleConnection &stale : staleConnections) {
                ClientSession removed{};
                if (m_sessions.RemoveConnection(stale.conn, &removed)) {
                    removedSessions.emplace_back(stale, std::move(removed));
                }
            }
        }

        for (const auto &[stale, session] : removedSessions) {
            std::cout << "[cleanup] remove conn=" << stale.conn << " user=" << session.username;
            if (stale.haveInfo) {
                std::cout << " state=" << ConnectionStateName(stale.info.m_eState)
                          << " endReason=" << stale.info.m_eEndReason;
                if (stale.info.m_szEndDebug[0] != '\0') {
                    std::cout << " debug=\"" << stale.info.m_szEndDebug << "\"";
                }
            } else {
                std::cout << " state=missing_info";
            }
            std::cout << "\n";
            m_hooks.teardownClientSession(stale.conn, session, "server cleanup", true);
        }
    }
}
