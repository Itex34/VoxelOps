#include "../ServerRuntime.hpp"

bool ServerRuntime::RunScoreboardPhase(
    std::chrono::steady_clock::time_point &nextScoreboardBroadcastAt) {
    const auto kScoreboardBroadcastInterval = std::chrono::seconds(1);
    const auto scoreboardNow = std::chrono::steady_clock::now();
    if (scoreboardNow < nextScoreboardBroadcastAt) {
        return false;
    }

    std::string scoreboardPayload;
    std::string endAnnouncementPayload;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        int remainingSec = static_cast<int>(m_matchDuration.count());
        if (m_matchStarted) {
            const int64_t elapsedSec =
                std::chrono::duration_cast<std::chrono::seconds>(scoreboardNow - m_matchStartTime)
                    .count();
            const int64_t remainingSecRaw =
                static_cast<int64_t>(m_matchDuration.count()) - elapsedSec;
            remainingSec = static_cast<int>(std::max<int64_t>(0, remainingSecRaw));
        }

        if (m_matchStarted && !m_matchEnded && remainingSec <= 0) {
            m_matchEnded = true;

            struct WinnerCandidate {
                std::string username;
                uint32_t kills = 0;
            };
            std::vector<WinnerCandidate> candidates;
            candidates.reserve(m_clients.size());
            for (const auto &[_, session] : m_clients) {
                if (session.playerId == 0 || session.username.empty()) {
                    continue;
                }
                uint32_t kills = 0;
                auto scoreIt = m_matchScores.find(session.playerId);
                if (scoreIt != m_matchScores.end()) {
                    kills = scoreIt->second.kills;
                }
                candidates.push_back(WinnerCandidate{session.username, kills});
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const WinnerCandidate &a, const WinnerCandidate &b) {
                          if (a.kills != b.kills) {
                              return a.kills > b.kills;
                          }
                          return a.username < b.username;
                      });

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

        struct ScoreboardRow {
            std::string username;
            uint32_t kills = 0;
            uint32_t deaths = 0;
            int pingMs = -1;
        };

        std::vector<ScoreboardRow> rows;
        rows.reserve(m_clients.size());
        for (const auto &[conn, session] : m_clients) {
            if (session.playerId == 0 || session.username.empty()) {
                continue;
            }

            ScoreboardRow row;
            row.username = session.username;
            auto scoreIt = m_matchScores.find(session.playerId);
            if (scoreIt != m_matchScores.end()) {
                row.kills = scoreIt->second.kills;
                row.deaths = scoreIt->second.deaths;
            }

            SteamNetConnectionRealTimeStatus_t status{};
            const EResult pingResult =
                SteamNetworkingSockets()->GetConnectionRealTimeStatus(conn, &status, 0, nullptr);
            if (pingResult == k_EResultOK) {
                row.pingMs = status.m_nPing;
            }

            rows.push_back(std::move(row));
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

        scoreboardPayload.reserve(64 + rows.size() * 32);
        scoreboardPayload += "SCOREBOARD|";
        scoreboardPayload += std::to_string(remainingSec);
        scoreboardPayload += "|";
        scoreboardPayload += (m_matchEnded ? "1" : "0");
        scoreboardPayload += "|";
        scoreboardPayload += (m_matchStarted ? "1" : "0");
        scoreboardPayload += "|";
        scoreboardPayload += m_matchWinner.empty() ? "-" : m_matchWinner;
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
    }

    if (!endAnnouncementPayload.empty()) {
        std::string out;
        out.reserve(1 + endAnnouncementPayload.size());
        out.push_back(static_cast<char>(PacketType::Message));
        out += endAnnouncementPayload;
        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
    }

    if (!scoreboardPayload.empty()) {
        std::string out;
        out.reserve(1 + scoreboardPayload.size());
        out.push_back(static_cast<char>(PacketType::Message));
        out += scoreboardPayload;
        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
    }

    nextScoreboardBroadcastAt = scoreboardNow + kScoreboardBroadcastInterval;
    return true;
}
