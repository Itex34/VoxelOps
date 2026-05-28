#pragma once

#include "../../../Shared/player/PlayerID.hpp"
#include "../session/ClientSessionManager.hpp"
#include "../../game/player/PlayerManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class ReplicationPhase {
public:
    using ClientSession = ClientSessionManager::ClientSession;

    struct Hooks {
        std::function<void(
            const std::vector<std::pair<HSteamNetConnection, PlayerID>> &, uint32_t
        )> sendWorldItemSnapshots;
        std::function<void(HSteamNetConnection, const ClientSession &, const char *, bool)>
            teardownClientSession;
    };

    ReplicationPhase(
        std::mutex &mutex, ClientSessionManager &sessions, PlayerManager &playerManager, Hooks hooks
    );

    double RunSnapshotPhase(
        uint32_t serverTick,
        std::chrono::steady_clock::time_point &lastSnapshotTime,
        const std::chrono::duration<double> &snapshotInterval
    );

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    Hooks m_hooks;
};
