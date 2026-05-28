#pragma once

#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class GameplayPhase {
public:
    struct Hooks {
        std::function<bool(PlayerID, uint32_t &, uint32_t &)> getPlayerScore;
        std::function<void(const void *, uint32_t, HSteamNetConnection)> broadcastRaw;
    };

    GameplayPhase(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        std::chrono::steady_clock::time_point &matchStartTime,
        std::chrono::seconds &matchDuration,
        bool &matchStarted,
        bool &matchEnded,
        std::string &matchWinner,
        Hooks hooks
    );

    bool RunScoreboardPhase(std::chrono::steady_clock::time_point &nextScoreboardBroadcastAt);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    std::chrono::steady_clock::time_point &m_matchStartTime;
    std::chrono::seconds &m_matchDuration;
    bool &m_matchStarted;
    bool &m_matchEnded;
    std::string &m_matchWinner;
    Hooks m_hooks;
};
