#include "ServerNetwork.hpp"
#include "PacketValidation.hpp"
#include "ServerDiagnosticsFlags.hpp"

namespace {
constexpr auto kServerPerfLogInterval = std::chrono::seconds(1);
constexpr double kSlowServerLoopWarnUs = 12000.0;
constexpr double kSlowServerSimWarnUs = 5000.0;
constexpr uint32_t kServerTickRateHz = 60u;

} // namespace

void ServerNetwork::DispatchInboundPacket(HSteamNetConnection incoming, PacketType packetType,
                                          const void *data, uint32_t size,
                                          uint64_t &playerInputPacketsThisLoop,
                                          uint64_t &chunkRequestPacketsThisLoop) {
    switch (packetType) {
    case PacketType::ConnectRequest:
        HandleConnectRequest(incoming, data, size);
        return;
    case PacketType::Message:
        HandleMessagePacket(incoming, data, size);
        return;
    case PacketType::PlayerInput:
        HandlePlayerInputPacket(incoming, data, size, playerInputPacketsThisLoop);
        return;
    case PacketType::PlayerPosition:
        // Legacy packet still accepted on the wire but ignored by authoritative movement.
        return;
    case PacketType::ChunkRequest:
        HandleChunkRequestPacket(incoming, data, size, chunkRequestPacketsThisLoop);
        return;
    case PacketType::BlockPlaceRequest:
        HandleBlockPlaceRequestPacket(incoming, data, size);
        return;
    case PacketType::BlockBreakRequest:
        HandleBlockBreakRequestPacket(incoming, data, size);
        return;
    case PacketType::ShootRequest:
        HandleShootRequestPacket(incoming, data, size);
        return;
    case PacketType::InventoryActionRequest:
        HandleInventoryActionRequestPacket(incoming, data, size);
        return;
    default:
        return;
    }
}

void ServerNetwork::RunInboundMessagePhase(uint64_t &msgPacketsThisLoop,
                                           uint64_t &playerInputPacketsThisLoop,
                                           uint64_t &chunkRequestPacketsThisLoop,
                                           double &messageDrainUs) {
    constexpr size_t kMaxInboundMessagesPerLoop = 256;
    constexpr int64_t kInboundMessageBudgetUs = 3000;

    SteamNetworkingSockets()->RunCallbacks();

    // Receive messages on poll group (any connection assigned to it).
    // Drain with a bounded budget so message bursts do not starve simulation ticks.
    const auto messageDrainStart = std::chrono::steady_clock::now();
    SteamNetworkingMessage_t *pMsg = nullptr;
    size_t inboundMessagesProcessed = 0;
    const auto reachedMessageDrainBudget = [&]() {
        ++inboundMessagesProcessed;
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - messageDrainStart)
                                      .count();
        return elapsedUs >= kInboundMessageBudgetUs;
    };
    while (inboundMessagesProcessed < kMaxInboundMessagesPerLoop &&
           SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, &pMsg, 1) > 0 &&
           pMsg) {
        HSteamNetConnection incoming = pMsg->m_conn;
        const void *data = pMsg->m_pData;
        const uint32_t cb = pMsg->m_cbSize;

        if (cb < 1 || cb > NetPacket::kMaxInboundPacketBytes) {
            std::cerr << "[recv] invalid packet size=" << cb << " conn=" << incoming
                      << " (closing connection)\n";
            SteamNetworkingSockets()->CloseConnection(incoming, 0, "invalid packet size", false);
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        const PacketType packetType =
            static_cast<PacketType>(reinterpret_cast<const uint8_t *>(data)[0]);
        if (!NetPacket::IsInboundPacketSizeValid(packetType, cb)) {
            std::cerr << "[recv] packet size/type mismatch type=" << static_cast<int>(packetType)
                      << " size=" << cb << " conn=" << incoming << "\n";
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        if (IsInboundRateLimitExceeded(incoming, packetType, cb)) {
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
            continue;
        }

        ++msgPacketsThisLoop;
        DispatchInboundPacket(incoming, packetType, data, cb, playerInputPacketsThisLoop,
                              chunkRequestPacketsThisLoop);
        pMsg->Release();
        if (reachedMessageDrainBudget()) {
            break;
        }
    }

    messageDrainUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - messageDrainStart)
                                             .count());
}

void ServerNetwork::RunConnectionCleanupPhase() {
    // Optional: extra safeguard - check connection states for any connections left
    // (callback already handles most).
    std::vector<std::pair<HSteamNetConnection, ClientSession>> staleConnections;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            HSteamNetConnection conn = it->first;
            SteamNetConnectionInfo_t info;
            if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info) &&
                (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                 info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)) {
                staleConnections.emplace_back(conn, it->second);
                it = m_clients.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (const auto &[conn, session] : staleConnections) {
        std::cout << "[cleanup] remove conn=" << conn << " user=" << session.username << "\n";
        TeardownClientSession(conn, session, "server cleanup", true);
    }
}

uint64_t ServerNetwork::RunSimulationPhase(double &simAccumulator, uint32_t &serverTick,
                                           double &simUs, bool &simBacklog) {
    constexpr double kServerTickSeconds = 1.0 / static_cast<double>(kServerTickRateHz);
    constexpr size_t kMaxSimCatchupTicksPerLoop = 4;

    const auto simStart = std::chrono::steady_clock::now();
    uint64_t simTicksThisLoop = 0;
    while (simAccumulator >= kServerTickSeconds && simTicksThisLoop < kMaxSimCatchupTicksPerLoop) {
        m_playerManager.update(kServerTickSeconds, m_chunkManager);
        UpdateWorldItems(kServerTickSeconds);
        simAccumulator -= kServerTickSeconds;
        ++serverTick;
        m_serverTick.store(serverTick, std::memory_order_release);
        RecordLagCompFrame(serverTick);
        ++simTicksThisLoop;
    }
    simUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - simStart)
                                    .count());
    simBacklog = (simAccumulator >= kServerTickSeconds);
    return simTicksThisLoop;
}

void ServerNetwork::RunRespawnDiagnosticsPhase(uint64_t simTicksThisLoop, uint32_t serverTick) {
    std::vector<PlayerID> respawnedPlayers;
    if (simTicksThisLoop > 0) {
        const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
        std::unordered_set<PlayerID> seenPlayerIds;
        seenPlayerIds.reserve(players.size());
        for (const ServerPlayer &player : players) {
            seenPlayerIds.insert(player.id);
            const auto aliveIt = m_lastAliveByPlayerId.find(player.id);
            const bool wasAlive =
                (aliveIt != m_lastAliveByPlayerId.end()) ? aliveIt->second : player.isAlive;
            if (!wasAlive && player.isAlive) {
                respawnedPlayers.push_back(player.id);
            }
            m_lastAliveByPlayerId[player.id] = player.isAlive;
        }
        for (auto it = m_lastAliveByPlayerId.begin(); it != m_lastAliveByPlayerId.end();) {
            if (seenPlayerIds.find(it->first) == seenPlayerIds.end()) {
                it = m_lastAliveByPlayerId.erase(it);
            } else {
                ++it;
            }
        }
    }

    const auto rbDiagNow = std::chrono::steady_clock::now();
    if (!respawnedPlayers.empty()) {
        std::unordered_set<PlayerID> respawnedSet(respawnedPlayers.begin(), respawnedPlayers.end());
        std::vector<HSteamNetConnection> pipelineResets;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            pipelineResets.reserve(respawnedPlayers.size());
            for (auto &[conn, session] : m_clients) {
                if (respawnedSet.find(session.playerId) == respawnedSet.end()) {
                    continue;
                }
                session.streamedChunks.clear();
                session.pendingChunkData.clear();
                session.hasChunkInterest = false;
                session.chunkInterestDirty = true;
                session.nextChunkInterestUpdateAt = std::chrono::steady_clock::time_point::min();
                pipelineResets.push_back(conn);
            }
        }
        for (HSteamNetConnection conn : pipelineResets) {
            ClearChunkPipelineForConnection(conn);
        }
        for (PlayerID playerId : respawnedPlayers) {
            m_respawnDiagUntilByPlayer[playerId] = rbDiagNow + std::chrono::seconds(10);
            m_respawnDiagNextLogAtByPlayer[playerId] = rbDiagNow;
        }
        if (ServerDiagFlags::g_enableRespawnRubberbandDiagnostics.load(std::memory_order_acquire)) {
            std::cerr << "[rbdiag/server] respawn reset players=" << respawnedPlayers.size()
                      << " conns=" << pipelineResets.size() << "\n";
        }
    }

    if (ServerDiagFlags::g_enableRespawnRubberbandDiagnostics.load(std::memory_order_acquire) &&
        !m_respawnDiagUntilByPlayer.empty()) {
        for (auto it = m_respawnDiagUntilByPlayer.begin();
             it != m_respawnDiagUntilByPlayer.end();) {
            const PlayerID playerId = it->first;
            const auto until = it->second;
            if (rbDiagNow >= until) {
                it = m_respawnDiagUntilByPlayer.erase(it);
                m_respawnDiagNextLogAtByPlayer.erase(playerId);
                continue;
            }

            auto nextLogIt = m_respawnDiagNextLogAtByPlayer.find(playerId);
            if (nextLogIt == m_respawnDiagNextLogAtByPlayer.end()) {
                m_respawnDiagNextLogAtByPlayer[playerId] = rbDiagNow;
                ++it;
                continue;
            }

            if (rbDiagNow >= nextLogIt->second) {
                const auto playerOpt = m_playerManager.getPlayerCopy(playerId);
                if (!playerOpt.has_value()) {
                    it = m_respawnDiagUntilByPlayer.erase(it);
                    m_respawnDiagNextLogAtByPlayer.erase(playerId);
                    continue;
                }

                HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
                size_t streamedChunks = 0;
                size_t pendingChunkData = 0;
                bool hasChunkInterest = false;
                bool chunkInterestDirty = false;
                glm::ivec3 centerChunk(0);
                uint16_t viewDistance = 0;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    for (const auto &[sessionConn, session] : m_clients) {
                        if (session.playerId == playerId) {
                            conn = sessionConn;
                            streamedChunks = session.streamedChunks.size();
                            pendingChunkData = session.pendingChunkData.size();
                            hasChunkInterest = session.hasChunkInterest;
                            chunkInterestDirty = session.chunkInterestDirty;
                            centerChunk = session.interestCenterChunk;
                            viewDistance = session.viewDistance;
                            break;
                        }
                    }
                }

                const size_t sendQueueDepth = (conn != k_HSteamNetConnection_Invalid)
                                                  ? GetChunkSendQueueDepthForClient(conn)
                                                  : 0;
                const ServerPlayer &player = *playerOpt;
                std::cerr << "[rbdiag/server] heartbeat"
                          << " tick=" << serverTick << " playerId=" << player.id << " conn=" << conn
                          << " alive=" << (player.isAlive ? 1 : 0) << " hp=" << player.health
                          << " pos=(" << player.position.x << "," << player.position.y << ","
                          << player.position.z << ")"
                          << " vel=(" << player.velocity.x << "," << player.velocity.y << ","
                          << player.velocity.z << ")"
                          << " onGround=" << (player.onGround ? 1 : 0) << " lastProcessedInputTick="
                          << player.inputBuffer.lastProcessedInputTick()
                          << " pendingInputs=" << player.inputBuffer.pendingInputCount()
                          << " hasInput=" << (player.inputBuffer.hasReceivedInput() ? 1 : 0)
                          << " chunkInterest=" << (hasChunkInterest ? 1 : 0)
                          << " chunkDirty=" << (chunkInterestDirty ? 1 : 0) << " center=("
                          << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
                          << " viewDist=" << viewDistance << " streamedChunks=" << streamedChunks
                          << " pendingChunkData=" << pendingChunkData
                          << " sendQueueDepth=" << sendQueueDepth << "\n";

                nextLogIt->second = rbDiagNow + std::chrono::seconds(1);
            }

            ++it;
        }
    }
}

double ServerNetwork::RunSnapshotPhase(uint32_t serverTick,
                                       std::chrono::steady_clock::time_point &lastSnapshotTime,
                                       const std::chrono::duration<double> &snapshotInterval) {
    const auto snapshotNow = std::chrono::steady_clock::now();
    const auto snapshotStart = std::chrono::steady_clock::now();
    bool snapshotRan = false;
    if (snapshotNow - lastSnapshotTime >= snapshotInterval) {
        snapshotRan = true;
        lastSnapshotTime = snapshotNow;

        std::vector<std::pair<HSteamNetConnection, PlayerID>> recipients;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            recipients.reserve(m_clients.size());
            for (const auto &[conn, session] : m_clients) {
                if (session.playerId != 0) {
                    recipients.emplace_back(conn, session.playerId);
                }
            }
        }

        std::vector<HSteamNetConnection> staleRecipients;
        std::vector<PlayerID> recipientIds;
        recipientIds.reserve(recipients.size());
        for (const auto &[_, playerId] : recipients) {
            recipientIds.push_back(playerId);
        }
        std::vector<std::vector<uint8_t>> snapshots =
            m_playerManager.buildSnapshotsForRecipients(recipientIds, serverTick);

        const size_t snapshotCount = std::min(recipients.size(), snapshots.size());
        std::vector<std::pair<HSteamNetConnection, PlayerID>> activeRecipients;
        activeRecipients.reserve(snapshotCount);
        for (size_t i = 0; i < snapshotCount; ++i) {
            const HSteamNetConnection conn = recipients[i].first;
            const PlayerID recipientPlayerId = recipients[i].second;
            std::vector<uint8_t> &snapshot = snapshots[i];
            if (snapshot.empty()) {
                staleRecipients.push_back(conn);
                continue;
            }
            activeRecipients.emplace_back(conn, recipientPlayerId);
            SteamNetworkingSockets()->SendMessageToConnection(
                conn, snapshot.data(), static_cast<uint32_t>(snapshot.size()),
                k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        }
        if (!activeRecipients.empty()) {
            SendWorldItemSnapshots(activeRecipients, serverTick);
        }

        if (!staleRecipients.empty()) {
            std::vector<std::pair<HSteamNetConnection, ClientSession>> removedSessions;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                for (HSteamNetConnection conn : staleRecipients) {
                    auto it = m_clients.find(conn);
                    if (it != m_clients.end()) {
                        removedSessions.emplace_back(it->first, it->second);
                        m_clients.erase(it);
                    }
                }
            }

            for (const auto &[conn, session] : removedSessions) {
                TeardownClientSession(conn, session, "server player timeout", true);
            }
        }
    }

    return snapshotRan ? static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - snapshotStart)
                                                 .count())
                       : 0.0;
}

bool ServerNetwork::RunScoreboardPhase(
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

size_t ServerNetwork::RunChunkInterestPhase(bool simBacklog, double &chunkInterestUs) {
    constexpr size_t kChunkInterestUpdatesPerLoop = 4;
    const auto kChunkInterestUpdateInterval = std::chrono::milliseconds(100);

    chunkInterestUs = 0.0;
    if (simBacklog) {
        return 0;
    }

    struct ChunkInterestTask {
        HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
        glm::ivec3 centerChunk{0};
        uint16_t viewDistance = 0;
    };
    std::vector<ChunkInterestTask> chunkInterestTasks;
    const auto chunkInterestStart = std::chrono::steady_clock::now();
    const auto chunkInterestNow = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        chunkInterestTasks.reserve(
            std::min<size_t>(kChunkInterestUpdatesPerLoop, m_clients.size()));
        for (auto &[conn, session] : m_clients) {
            if (!session.hasChunkInterest) {
                continue;
            }
            if (!session.chunkInterestDirty &&
                chunkInterestNow < session.nextChunkInterestUpdateAt) {
                continue;
            }

            chunkInterestTasks.push_back(
                ChunkInterestTask{conn, session.interestCenterChunk, session.viewDistance});
            session.chunkInterestDirty = false;
            session.nextChunkInterestUpdateAt = chunkInterestNow + kChunkInterestUpdateInterval;

            if (chunkInterestTasks.size() >= kChunkInterestUpdatesPerLoop) {
                break;
            }
        }
    }
    for (const ChunkInterestTask &task : chunkInterestTasks) {
        UpdateChunkStreamingForClient(task.conn, task.centerChunk, task.viewDistance);
    }
    chunkInterestUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - chunkInterestStart)
                                              .count());
    return chunkInterestTasks.size();
}

size_t ServerNetwork::RunChunkSendPhase(bool simBacklog,
                                        std::chrono::steady_clock::time_point &nextChunkSendFlushAt,
                                        double &chunkSendUs) {
    constexpr size_t kChunkSendGlobalBudgetPerFlush = 8;
    constexpr size_t kChunkSendPerClientBudgetPerFlush = 4;
    const auto kChunkSendFlushInterval = std::chrono::milliseconds(16);

    chunkSendUs = 0.0;
    if (simBacklog) {
        return 0;
    }

    const auto chunkSendNow = std::chrono::steady_clock::now();
    if (chunkSendNow < nextChunkSendFlushAt) {
        return 0;
    }

    const auto chunkSendStart = std::chrono::steady_clock::now();
    const size_t chunksSentThisLoop =
        FlushChunkSendQueues(kChunkSendGlobalBudgetPerFlush, kChunkSendPerClientBudgetPerFlush);
    chunkSendUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - chunkSendStart)
                                          .count());
    nextChunkSendFlushAt = chunkSendNow + kChunkSendFlushInterval;
    return chunksSentThisLoop;
}

size_t ServerNetwork::RunCollisionPrewarmPhase(
    bool simBacklog, std::chrono::steady_clock::time_point &nextCollisionPrewarmAt,
    double &collisionPrewarmUs) {
    constexpr int kCollisionPrewarmRadiusXZ = 1;
    constexpr int kCollisionPrewarmRadiusY = 1;
    constexpr size_t kMaxCollisionPrewarmGenerationsPerLoop = 8;
    constexpr int64_t kCollisionPrewarmBudgetUs = 1500;
    const auto kCollisionPrewarmInterval = std::chrono::milliseconds(50);

    collisionPrewarmUs = 0.0;
    if (simBacklog) {
        // Sim tick is behind: defer background world work and retry once backlog clears.
        nextCollisionPrewarmAt = std::chrono::steady_clock::now();
        return 0;
    }

    const auto prewarmNow = std::chrono::steady_clock::now();
    if (prewarmNow < nextCollisionPrewarmAt) {
        return 0;
    }

    const auto collisionPrewarmStart = prewarmNow;
    size_t collisionPrewarmGeneratedThisLoop = 0;
    std::vector<PlayerID> activePlayerIds;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        activePlayerIds.reserve(m_clients.size());
        for (const auto &[_, session] : m_clients) {
            if (session.playerId != 0) {
                activePlayerIds.push_back(session.playerId);
            }
        }
    }

    for (PlayerID playerId : activePlayerIds) {
        if (collisionPrewarmGeneratedThisLoop >= kMaxCollisionPrewarmGenerationsPerLoop) {
            break;
        }
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - collisionPrewarmStart)
                                      .count();
        if (elapsedUs >= kCollisionPrewarmBudgetUs) {
            break;
        }

        const auto playerOpt = m_playerManager.getPlayerCopy(playerId);
        if (!playerOpt.has_value()) {
            continue;
        }

        const ServerPlayer &player = *playerOpt;
        const glm::ivec3 playerWorldPos(static_cast<int>(std::floor(player.position.x)),
                                        static_cast<int>(std::floor(player.position.y)),
                                        static_cast<int>(std::floor(player.position.z)));
        const glm::ivec3 centerChunk = m_chunkManager.worldToChunkPos(playerWorldPos);

        bool hitLoopBudget = false;
        for (int dx = -kCollisionPrewarmRadiusXZ; dx <= kCollisionPrewarmRadiusXZ && !hitLoopBudget;
             ++dx) {
            for (int dz = -kCollisionPrewarmRadiusXZ;
                 dz <= kCollisionPrewarmRadiusXZ && !hitLoopBudget; ++dz) {
                for (int dy = -kCollisionPrewarmRadiusY; dy <= kCollisionPrewarmRadiusY; ++dy) {
                    const int64_t innerElapsedUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - collisionPrewarmStart)
                            .count();
                    if (innerElapsedUs >= kCollisionPrewarmBudgetUs) {
                        hitLoopBudget = true;
                        break;
                    }

                    const glm::ivec3 chunkPos(centerChunk.x + dx, centerChunk.y + dy,
                                              centerChunk.z + dz);
                    if (!m_chunkManager.inBounds(chunkPos)) {
                        continue;
                    }
                    if (m_chunkManager.hasChunkLoaded(chunkPos)) {
                        continue;
                    }
                    m_chunkManager.generateTerrainChunkAt(chunkPos);
                    ++collisionPrewarmGeneratedThisLoop;
                    if (collisionPrewarmGeneratedThisLoop >=
                        kMaxCollisionPrewarmGenerationsPerLoop) {
                        hitLoopBudget = true;
                        break;
                    }
                }
            }
        }
    }
    collisionPrewarmUs =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - collisionPrewarmStart)
                                .count());
    nextCollisionPrewarmAt = std::chrono::steady_clock::now() + kCollisionPrewarmInterval;
    return collisionPrewarmGeneratedThisLoop;
}

void ServerNetwork::ApplyLoopPacingPhase(bool simBacklog) {
    if (simBacklog) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ServerNetwork::MainLoop() {
    auto lastFrameTime = std::chrono::steady_clock::now();
    auto lastSnapshotTime = lastFrameTime;
    constexpr uint32_t kSnapshotSendRateHz =
        60u; // Match server tick rate for smooth reconciliation (CS:GO/Valorant style)
    constexpr double kSnapshotSendSeconds = 1.0 / static_cast<double>(kSnapshotSendRateHz);
    const auto snapshotInterval = std::chrono::duration<double>(kSnapshotSendSeconds);

    double simAccumulator = 0.0;
    uint32_t serverTick = 0;
    auto nextChunkSendFlushAt = std::chrono::steady_clock::now();
    auto nextCollisionPrewarmAt = std::chrono::steady_clock::now();
    auto perfWindowStart = std::chrono::steady_clock::now();
    auto nextScoreboardBroadcastAt = std::chrono::steady_clock::now();

    uint64_t perfLoops = 0;
    uint64_t perfMessages = 0;
    uint64_t perfPlayerInputs = 0;
    uint64_t perfChunkRequests = 0;
    uint64_t perfSimTicks = 0;
    uint64_t perfCollisionPrewarmGenerated = 0;
    uint64_t perfChunkInterestTasks = 0;
    uint64_t perfChunksSent = 0;
    uint64_t perfScoreboardBroadcasts = 0;
    double perfLoopUsTotal = 0.0;
    double perfLoopUsMax = 0.0;
    double perfMessageDrainUsTotal = 0.0;
    double perfSimUsTotal = 0.0;
    double perfCollisionPrewarmUsTotal = 0.0;
    double perfSnapshotUsTotal = 0.0;
    double perfChunkInterestUsTotal = 0.0;
    double perfChunkSendUsTotal = 0.0;

    while (!m_quit) {
        const auto loopStart = std::chrono::steady_clock::now();

        uint64_t msgPacketsThisLoop = 0;
        uint64_t playerInputPacketsThisLoop = 0;
        uint64_t chunkRequestPacketsThisLoop = 0;
        double messageDrainUs = 0.0;
        double simUs = 0.0;
        double snapshotUs = 0.0;
        double chunkInterestUs = 0.0;
        double chunkSendUs = 0.0;
        double collisionPrewarmUs = 0.0;
        bool simBacklog = false;

        const auto frameNow = std::chrono::steady_clock::now();
        double deltaSeconds = std::chrono::duration<double>(frameNow - lastFrameTime).count();
        if (deltaSeconds > 0.25) {
            deltaSeconds = 0.25;
        }
        lastFrameTime = frameNow;
        simAccumulator += deltaSeconds;

        RunInboundMessagePhase(msgPacketsThisLoop, playerInputPacketsThisLoop,
                               chunkRequestPacketsThisLoop, messageDrainUs);
        RunConnectionCleanupPhase();
        const uint64_t simTicksThisLoop =
            RunSimulationPhase(simAccumulator, serverTick, simUs, simBacklog);
        RunRespawnDiagnosticsPhase(simTicksThisLoop, serverTick);
        snapshotUs = RunSnapshotPhase(serverTick, lastSnapshotTime, snapshotInterval);
        const bool scoreboardBroadcastedThisLoop = RunScoreboardPhase(nextScoreboardBroadcastAt);
        const size_t chunkInterestTasksThisLoop =
            RunChunkInterestPhase(simBacklog, chunkInterestUs);
        const size_t chunksSentThisLoop =
            RunChunkSendPhase(simBacklog, nextChunkSendFlushAt, chunkSendUs);
        const size_t collisionPrewarmGeneratedThisLoop =
            RunCollisionPrewarmPhase(simBacklog, nextCollisionPrewarmAt, collisionPrewarmUs);

        const double loopUs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - loopStart)
                                    .count());
        if (loopUs > perfLoopUsMax) {
            perfLoopUsMax = loopUs;
        }
        ++perfLoops;
        perfMessages += msgPacketsThisLoop;
        perfPlayerInputs += playerInputPacketsThisLoop;
        perfChunkRequests += chunkRequestPacketsThisLoop;
        perfSimTicks += simTicksThisLoop;
        perfCollisionPrewarmGenerated += collisionPrewarmGeneratedThisLoop;
        perfChunkInterestTasks += chunkInterestTasksThisLoop;
        perfChunksSent += chunksSentThisLoop;
        if (scoreboardBroadcastedThisLoop) {
            ++perfScoreboardBroadcasts;
        }
        perfLoopUsTotal += loopUs;
        perfMessageDrainUsTotal += messageDrainUs;
        perfSimUsTotal += simUs;
        perfCollisionPrewarmUsTotal += collisionPrewarmUs;
        perfSnapshotUsTotal += snapshotUs;
        perfChunkInterestUsTotal += chunkInterestUs;
        perfChunkSendUsTotal += chunkSendUs;

        if (ServerDiagFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (loopUs >= kSlowServerLoopWarnUs || simUs >= kSlowServerSimWarnUs)) {
            std::cerr << "[perf/server] slow loopUs=" << loopUs << " simUs=" << simUs
                      << " msgDrainUs=" << messageDrainUs << " prewarmUs=" << collisionPrewarmUs
                      << " snapshotUs=" << snapshotUs << " chunkInterestUs=" << chunkInterestUs
                      << " chunkSendUs=" << chunkSendUs << " simTicks=" << simTicksThisLoop
                      << " msgs=" << msgPacketsThisLoop << " inputs=" << playerInputPacketsThisLoop
                      << " chunkReq=" << chunkRequestPacketsThisLoop
                      << " prewarmGenerated=" << collisionPrewarmGeneratedThisLoop
                      << " chunkInterestTasks=" << chunkInterestTasksThisLoop
                      << " chunksSent=" << chunksSentThisLoop << "\n";
        }

        const auto perfNow = std::chrono::steady_clock::now();
        if (ServerDiagFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (perfNow - perfWindowStart) >= kServerPerfLogInterval) {
            const double loops = (perfLoops > 0) ? static_cast<double>(perfLoops) : 1.0;
            std::cerr << "[perf/server] 1s loops=" << perfLoops
                      << " avgLoopMs=" << (perfLoopUsTotal / loops) / 1000.0
                      << " maxLoopMs=" << perfLoopUsMax / 1000.0
                      << " avgMsgDrainMs=" << (perfMessageDrainUsTotal / loops) / 1000.0
                      << " avgSimMs=" << (perfSimUsTotal / loops) / 1000.0
                      << " avgPrewarmMs=" << (perfCollisionPrewarmUsTotal / loops) / 1000.0
                      << " avgSnapshotMs=" << (perfSnapshotUsTotal / loops) / 1000.0
                      << " avgChunkInterestMs=" << (perfChunkInterestUsTotal / loops) / 1000.0
                      << " avgChunkSendMs=" << (perfChunkSendUsTotal / loops) / 1000.0
                      << " simTicks=" << perfSimTicks << " msgs=" << perfMessages
                      << " inputs=" << perfPlayerInputs << " chunkReq=" << perfChunkRequests
                      << " prewarmGenerated=" << perfCollisionPrewarmGenerated
                      << " chunkInterestTasks=" << perfChunkInterestTasks
                      << " chunksSent=" << perfChunksSent
                      << " scoreboardBroadcasts=" << perfScoreboardBroadcasts << "\n";

            perfWindowStart = perfNow;
            perfLoops = 0;
            perfMessages = 0;
            perfPlayerInputs = 0;
            perfChunkRequests = 0;
            perfSimTicks = 0;
            perfCollisionPrewarmGenerated = 0;
            perfChunkInterestTasks = 0;
            perfChunksSent = 0;
            perfScoreboardBroadcasts = 0;
            perfLoopUsTotal = 0.0;
            perfLoopUsMax = 0.0;
            perfMessageDrainUsTotal = 0.0;
            perfSimUsTotal = 0.0;
            perfCollisionPrewarmUsTotal = 0.0;
            perfSnapshotUsTotal = 0.0;
            perfChunkInterestUsTotal = 0.0;
            perfChunkSendUsTotal = 0.0;
        }

        ApplyLoopPacingPhase(simBacklog);
    }
}

// Broadcast raw payload to everyone except `except`
void ServerNetwork::SetDebugLoggingEnabled(bool enabled) {
    ServerDiagFlags::SetAllEnabled(enabled);
    m_playerManager.SetDebugLoggingEnabled(enabled);
    std::cout << "[debug] diagnostics " << (enabled ? "enabled" : "disabled") << "\n";
}

bool ServerNetwork::IsDebugLoggingEnabled() {
    if (ServerDiagFlags::IsAnyEnabled()) {
        return true;
    }
    return m_playerManager.IsDebugLoggingEnabled();
}
