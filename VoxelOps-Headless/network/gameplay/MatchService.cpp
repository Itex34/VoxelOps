#include "MatchService.hpp"

#include "../../game/combat/CombatFeedback.hpp"

void MatchService::ResetForNewRun() {
    m_matchStartTime = std::chrono::steady_clock::now();
    m_matchStarted = false;
    m_matchEnded = false;
    m_matchWinner.clear();
    m_scores.clear();
}

void MatchService::ClearScores() {
    m_scores.clear();
}

void MatchService::OnPlayerDetached(PlayerID playerId) {
    m_scores.erase(playerId);
}

void MatchService::OnSessionAttached(PlayerID playerId, size_t activePlayers) {
    m_scores[playerId] = Score{};
    if (!m_matchStarted && activePlayers >= 2) {
        m_matchStarted = true;
        m_matchStartTime = std::chrono::steady_clock::now();
        m_matchEnded = false;
        m_matchWinner.clear();
    }
}

void MatchService::ApplyKillScore(PlayerID killerId, PlayerID victimId) {
    CombatFeedback::ApplyKillScore(m_scores, m_matchEnded, killerId, victimId);
}

bool MatchService::GetPlayerScore(PlayerID playerId, uint32_t &kills, uint32_t &deaths) const {
    const auto it = m_scores.find(playerId);
    if (it == m_scores.end()) {
        return false;
    }
    kills = it->second.kills;
    deaths = it->second.deaths;
    return true;
}

std::chrono::steady_clock::time_point &MatchService::MatchStartTimeRef() {
    return m_matchStartTime;
}

std::chrono::seconds &MatchService::MatchDurationRef() {
    return m_matchDuration;
}

bool &MatchService::MatchStartedRef() {
    return m_matchStarted;
}

bool &MatchService::MatchEndedRef() {
    return m_matchEnded;
}

std::string &MatchService::MatchWinnerRef() {
    return m_matchWinner;
}
