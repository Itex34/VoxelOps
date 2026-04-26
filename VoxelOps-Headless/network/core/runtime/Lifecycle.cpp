#include "../Runtime.hpp"

void Runtime::TeardownClientSession(HSteamNetConnection conn, const ClientSession &session,
                                          const char *closeReason, bool closeConnection) {
    ClearChunkPipelineForConnection(conn);

    if (session.playerId != 0) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_connectionByPlayerId.erase(session.playerId);
            m_matchScores.erase(session.playerId);
        }
        m_playerManager.removePlayer(session.playerId);
        InvalidateCombatSnapshotCache();
    }

    if (!session.username.empty()) {
        std::string out;
        out.push_back(static_cast<char>(PacketType::ClientDisconnect));
        out += session.username;
        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), conn);
    }

    if (closeConnection) {
        SteamNetworkingSockets()->CloseConnection(conn, 0, closeReason, false);
    }
}
