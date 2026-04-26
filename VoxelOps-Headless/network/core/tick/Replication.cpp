#include "../ServerRuntime.hpp"

double ServerRuntime::RunSnapshotPhase(uint32_t serverTick,
                                       std::chrono::steady_clock::time_point &lastSnapshotTime,
                                       const std::chrono::duration<double> &snapshotInterval) {
    const auto snapshotNow = std::chrono::steady_clock::now();
    const auto snapshotStart = std::chrono::steady_clock::now();
    bool snapshotRan = false;
    if (snapshotNow - lastSnapshotTime >= snapshotInterval) {
        snapshotRan = true;
        lastSnapshotTime = snapshotNow;

        std::vector<std::pair<HSteamNetConnection, PlayerID>> recipients;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            recipients.reserve(m_clients.size());
            for (const auto &[conn, session] : m_clients) {
                if (session.playerId != 0) {
                    recipients.emplace_back(conn, session.playerId);
                }
            }
        }

        std::vector<HSteamNetConnection> staleRecipients;
        std::vector<PlayerID> recipientIds;
        recipientIds.reserve(recipients.size());
        for (const auto &[_, playerId] : recipients) {
            recipientIds.push_back(playerId);
        }
        std::vector<std::vector<uint8_t>> snapshots =
            m_playerManager.buildSnapshotsForRecipients(recipientIds, serverTick);

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
                conn, snapshot.data(), static_cast<uint32_t>(snapshot.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
        if (!activeRecipients.empty()) {
            SendWorldItemSnapshots(activeRecipients, serverTick);
        }

        if (!staleRecipients.empty()) {
            std::vector<std::pair<HSteamNetConnection, ClientSession>> removedSessions;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                for (HSteamNetConnection conn : staleRecipients) {
                    auto it = m_clients.find(conn);
                    if (it != m_clients.end()) {
                        removedSessions.emplace_back(it->first, it->second);
                        m_clients.erase(it);
                    }
                }
            }

            for (const auto &[conn, session] : removedSessions) {
                TeardownClientSession(conn, session, "server player timeout", true);
            }
        }
    }

    return snapshotRan ? static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - snapshotStart)
                                                 .count())
                       : 0.0;
}
