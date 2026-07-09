#include "ReplicationPhase.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace {
    std::chrono::duration<double> SnapshotIntervalForRecipientCount(size_t recipients) {
        if (recipients >= 33) {
            return std::chrono::duration<double>(1.0 / 30.0);
        }
        return std::chrono::duration<double>(1.0 / 60.0);
    }
} // namespace

ReplicationPhase::ReplicationPhase(
    std::mutex &mutex, ClientSessionManager &sessions, PlayerManager &playerManager, Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_hooks(std::move(hooks)) {}

double ReplicationPhase::RunSnapshotPhase(
    uint32_t serverTick,
    std::chrono::steady_clock::time_point &lastSnapshotTime,
    const std::chrono::duration<double> &snapshotInterval
) {
    const auto snapshotNow = std::chrono::steady_clock::now();
    const auto snapshotStart = std::chrono::steady_clock::now();
    bool snapshotRan = false;
    size_t registeredRecipientCount = 0;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "ReplicationPhase::RunSnapshotPhase.countRecipients"
        );
        registeredRecipientCount = m_sessions.CountRegisteredSessions();
    }

    const auto effectiveSnapshotInterval =
        std::max(snapshotInterval, SnapshotIntervalForRecipientCount(registeredRecipientCount));
    if (snapshotNow - lastSnapshotTime >= effectiveSnapshotInterval) {
        snapshotRan = true;
        lastSnapshotTime = snapshotNow;

        std::vector<std::pair<HSteamNetConnection, PlayerID>> recipients;
        std::vector<std::pair<PlayerID, uint16_t>> recipientInterests;
        {
            auto lk =
                LockWaitTelemetry::AcquireSessionLock(m_mutex, "ReplicationPhase::RunSnapshotPhase");
            recipients.reserve(m_sessions.size());
            recipientInterests.reserve(m_sessions.size());
            for (const auto &[conn, session] : m_sessions) {
                if (session.playerId != 0) {
                    recipients.emplace_back(conn, session.playerId);
                    recipientInterests.emplace_back(session.playerId, session.viewDistance);
                }
            }
        }

        std::vector<HSteamNetConnection> staleRecipients;
        std::vector<std::vector<uint8_t>> snapshots =
            m_playerManager.buildSnapshotsForRecipients(recipientInterests, serverTick);

        const size_t snapshotCount = std::min(recipients.size(), snapshots.size());
        std::vector<std::pair<HSteamNetConnection, PlayerID>> activeRecipients;
        activeRecipients.reserve(snapshotCount);
        for (size_t i = 0; i < snapshotCount; ++i) {
            const HSteamNetConnection conn = recipients[i].first;
            const PlayerID recipientPlayerId = recipients[i].second;
            std::vector<uint8_t> &snapshot = snapshots[i];
            if (snapshot.empty()) {
                staleRecipients.push_back(conn);
                continue;
            }
            activeRecipients.emplace_back(conn, recipientPlayerId);
            SteamNetworkingSockets()->SendMessageToConnection(
                conn,
                snapshot.data(),
                static_cast<uint32_t>(snapshot.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay,
                nullptr
            );
        }
        if (!activeRecipients.empty()) {
            m_hooks.sendWorldItemSnapshots(activeRecipients, serverTick);
        }

        if (!staleRecipients.empty()) {
            std::vector<std::pair<HSteamNetConnection, ClientSession>> removedSessions;
            {
                auto lk = LockWaitTelemetry::AcquireSessionLock(
                    m_mutex, "ReplicationPhase::RunSnapshotPhase.removeStaleRecipients"
                );
                for (HSteamNetConnection conn : staleRecipients) {
                    ClientSession removed{};
                    if (m_sessions.RemoveConnection(conn, &removed)) {
                        removedSessions.emplace_back(conn, std::move(removed));
                    }
                }
            }

            for (const auto &[conn, session] : removedSessions) {
                m_hooks.teardownClientSession(conn, session, "server player timeout", true);
            }
        }
    }

    return snapshotRan ? static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - snapshotStart
                         )
                                                 .count())
                       : 0.0;
}
