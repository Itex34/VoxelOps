#include "SessionLifecycleService.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <string>
#include <utility>

SessionLifecycleService::SessionLifecycleService(
    std::mutex &mutex, ClientSessionManager &sessions, PlayerManager &playerManager, Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_hooks(std::move(hooks)) {}

void SessionLifecycleService::TeardownClientSession(
    HSteamNetConnection conn,
    const ClientSession &session,
    const char *closeReason,
    bool closeConnection
) {
    m_hooks.clearChunkPipelineForConnection(conn);

    if (session.playerId != 0) {
        {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_mutex, "SessionLifecycleService::TeardownClientSession"
            );
            m_sessions.UnbindPlayer(session.playerId);
            m_hooks.onPlayerDetached(session.playerId);
        }
        m_playerManager.removePlayer(session.playerId);
        m_hooks.invalidateCombatSnapshotCache();
    }

    if (!session.username.empty()) {
        std::string out;
        out.push_back(static_cast<char>(PacketType::ClientDisconnect));
        out += session.username;
        m_hooks.broadcastRaw(out.data(), static_cast<uint32_t>(out.size()), conn);
    }

    if (closeConnection) {
        SteamNetworkingSockets()->CloseConnection(conn, 0, closeReason, false);
    }
}
