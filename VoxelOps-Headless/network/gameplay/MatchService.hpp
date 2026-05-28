#pragma once

#include "../../../Shared/player/PlayerID.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

class MatchService {
public:
    struct Score {
        uint32_t kills = 0;
        uint32_t deaths = 0;
    };

    void ResetForNewRun();
    void ClearScores();
    void OnPlayerDetached(PlayerID playerId);
    void OnSessionAttached(PlayerID playerId, size_t activePlayers);
    void ApplyKillScore(PlayerID killerId, PlayerID victimId);
    bool GetPlayerScore(PlayerID playerId, uint32_t &kills, uint32_t &deaths) const;

    std::chrono::steady_clock::time_point &MatchStartTimeRef();
    std::chrono::seconds &MatchDurationRef();
    bool &MatchStartedRef();
    bool &MatchEndedRef();
    std::string &MatchWinnerRef();

private:
    std::unordered_map<PlayerID, Score> m_scores;
    std::chrono::steady_clock::time_point m_matchStartTime = std::chrono::steady_clock::now();
    std::chrono::seconds m_matchDuration{600};
    bool m_matchStarted = false;
    bool m_matchEnded = false;
    std::string m_matchWinner;
};
