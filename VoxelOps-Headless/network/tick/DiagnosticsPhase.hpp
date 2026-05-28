#pragma once

#include "../session/ClientSessionManager.hpp"
#include "../../game/player/PlayerManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <glm/ext/vector_int3.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class DiagnosticsPhase {
public:
    struct Hooks {
        std::function<void(HSteamNetConnection)> clearChunkPipelineForConnection;
        std::function<size_t(HSteamNetConnection)> getChunkSendQueueDepthForClient;
    };

    DiagnosticsPhase(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        std::unordered_map<PlayerID, bool> &lastAliveByPlayerId,
        std::unordered_map<PlayerID, std::chrono::steady_clock::time_point> &respawnDiagUntilByPlayer,
        std::unordered_map<PlayerID, std::chrono::steady_clock::time_point>
            &respawnDiagNextLogAtByPlayer,
        Hooks hooks
    );

    void RunRespawnDiagnosticsPhase(uint64_t simTicksThisLoop, uint32_t serverTick);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    std::unordered_map<PlayerID, bool> &m_lastAliveByPlayerId;
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point> &m_respawnDiagUntilByPlayer;
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point>
        &m_respawnDiagNextLogAtByPlayer;
    Hooks m_hooks;
};
