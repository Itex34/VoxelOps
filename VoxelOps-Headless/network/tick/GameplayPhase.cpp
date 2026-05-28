#include "GameplayPhase.hpp"

#include "../../../Shared/network/PacketType.hpp"
#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <algorithm>
#include <utility>

GameplayPhase::GameplayPhase(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    std::chrono::steady_clock::time_point &matchStartTime,
    std::chrono::seconds &matchDuration,
    bool &matchStarted,
    bool &matchEnded,
    std::string &matchWinner,
    Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_matchStartTime(matchStartTime)
    , m_matchDuration(matchDuration)
    , m_matchStarted(matchStarted)
    , m_matchEnded(matchEnded)
    , m_matchWinner(matchWinner)
    , m_hooks(std::move(hooks)) {}

bool GameplayPhase::RunScoreboardPhase(
    std::chrono::steady_clock::time_point &nextScoreboardBroadcastAt
) {
    const auto kScoreboardBroadcastInterval = std::chrono::seconds(1);
    const auto scoreboardNow = std::chrono::steady_clock::now();
    if (scoreboardNow < nextScoreboardBroadcastAt) {
        return false;
    }

    struct ScoreboardRow {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        std::string username;
        uint32_t kills = 0;
        uint32_t deaths = 0;
        int pingMs = -1;
    };
    std::vector<ScoreboardRow> rows;
    std::string matchWinner;
    bool matchStarted = false;
    bool matchEnded = false;
    int remainingSec = static_cast<int>(m_matchDuration.count());
    std::string endAnnouncementPayload;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(m_mutex, "GameplayPhase::RunScoreboardPhase");

        if (m_matchStarted) {
            const int64_t elapsedSec =
                std::chrono::duration_cast<std::chrono::seconds>(scoreboardNow - m_matchStartTime)
                    .count();
            const int64_t remainingSecRaw = static_cast<int64_t>(m_matchDuration.count()) - elapsedSec;
            remainingSec = static_cast<int>(std::max<int64_t>(0, remainingSecRaw));
        }

        if (m_matchStarted && !m_matchEnded && remainingSec <= 0) {
            m_matchEnded = true;

            struct WinnerCandidate {
                std::string username;
                uint32_t kills = 0;
            };
            std::vector<WinnerCandidate> candidates;
            candidates.reserve(m_sessions.size());
            for (const auto &[_, session] : m_sessions) {
                if (session.playerId == 0 || session.username.empty()) {
                    continue;
                }
                uint32_t kills = 0;
                uint32_t deaths = 0;
                (void)m_hooks.getPlayerScore(session.playerId, kills, deaths);
                candidates.push_back(WinnerCandidate{session.username, kills});
            }
            std::sort(
                candidates.begin(),
                candidates.end(),
                [](const WinnerCandidate &a, const WinnerCandidate &b) {
                    if (a.kills != b.kills) {
                        return a.kills > b.kills;
                    }
                    return a.username < b.username;
                }
            );

            if (candidates.empty()) {
                m_matchWinner = "No winner";
            } else if (candidates.size() >= 2 && candidates[0].kills == candidates[1].kills) {
                m_matchWinner = "Tie";
            } else {
                m_matchWinner = candidates[0].username;
            }

            endAnnouncementPayload = "MATCH_END|";
            endAnnouncementPayload += m_matchWinner;
        }

        rows.reserve(m_sessions.size());
        for (const auto &[conn, session] : m_sessions) {
            if (session.playerId == 0 || session.username.empty()) {
                continue;
            }
            ScoreboardRow row;
            row.conn = conn;
            row.username = session.username;
            (void)m_hooks.getPlayerScore(session.playerId, row.kills, row.deaths);
            rows.push_back(std::move(row));
        }

        matchWinner = m_matchWinner;
        matchStarted = m_matchStarted;
        matchEnded = m_matchEnded;
    }

    for (ScoreboardRow &row : rows) {
        SteamNetConnectionRealTimeStatus_t status{};
        const EResult pingResult =
            SteamNetworkingSockets()->GetConnectionRealTimeStatus(row.conn, &status, 0, nullptr);
        if (pingResult == k_EResultOK) {
            row.pingMs = status.m_nPing;
        }
    }

    std::sort(rows.begin(), rows.end(), [](const ScoreboardRow &a, const ScoreboardRow &b) {
        if (a.kills != b.kills) {
            return a.kills > b.kills;
        }
        if (a.deaths != b.deaths) {
            return a.deaths < b.deaths;
        }
        return a.username < b.username;
    });

    std::string scoreboardPayload;
    scoreboardPayload.reserve(64 + rows.size() * 32);
    scoreboardPayload += "SCOREBOARD|";
    scoreboardPayload += std::to_string(remainingSec);
    scoreboardPayload += "|";
    scoreboardPayload += (matchEnded ? "1" : "0");
    scoreboardPayload += "|";
    scoreboardPayload += (matchStarted ? "1" : "0");
    scoreboardPayload += "|";
    scoreboardPayload += matchWinner.empty() ? "-" : matchWinner;
    scoreboardPayload += "|";
    scoreboardPayload += std::to_string(rows.size());
    for (const ScoreboardRow &row : rows) {
        scoreboardPayload += "|";
        scoreboardPayload += row.username;
        scoreboardPayload += ",";
        scoreboardPayload += std::to_string(row.kills);
        scoreboardPayload += ",";
        scoreboardPayload += std::to_string(row.deaths);
        scoreboardPayload += ",";
        scoreboardPayload += std::to_string(row.pingMs);
    }

    if (!endAnnouncementPayload.empty()) {
        std::string out;
        out.reserve(1 + endAnnouncementPayload.size());
        out.push_back(static_cast<char>(PacketType::Message));
        out += endAnnouncementPayload;
        m_hooks.broadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
    }

    if (!scoreboardPayload.empty()) {
        std::string out;
        out.reserve(1 + scoreboardPayload.size());
        out.push_back(static_cast<char>(PacketType::Message));
        out += scoreboardPayload;
        m_hooks.broadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
    }

    nextScoreboardBroadcastAt = scoreboardNow + kScoreboardBroadcastInterval;
    return true;
}
