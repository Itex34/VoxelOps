#pragma once

#include "../../../Shared/network/PacketType.hpp"
#include "../../../Shared/player/PlayerID.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <functional>
#include <mutex>

class SessionLifecycleService {
public:
    using ClientSession = ClientSessionManager::ClientSession;

    struct Hooks {
        std::function<void(HSteamNetConnection)> clearChunkPipelineForConnection;
        std::function<void(PlayerID)> onPlayerDetached;
        std::function<void()> invalidateCombatSnapshotCache;
        std::function<void(const void *, uint32_t, HSteamNetConnection)> broadcastRaw;
    };

    SessionLifecycleService(
        std::mutex &mutex, ClientSessionManager &sessions, PlayerManager &playerManager, Hooks hooks
    );

    void TeardownClientSession(
        HSteamNetConnection conn,
        const ClientSession &session,
        const char *closeReason,
        bool closeConnection
    );

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    Hooks m_hooks;
};
