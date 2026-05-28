#include "DiagnosticsPhase.hpp"

#include "../core/LockWaitTelemetry.hpp"
#include "../core/DiagnosticsFlags.hpp"

#include <iostream>
#include <unordered_set>
#include <utility>

DiagnosticsPhase::DiagnosticsPhase(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    std::unordered_map<PlayerID, bool> &lastAliveByPlayerId,
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point> &respawnDiagUntilByPlayer,
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point>
        &respawnDiagNextLogAtByPlayer,
    Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_lastAliveByPlayerId(lastAliveByPlayerId)
    , m_respawnDiagUntilByPlayer(respawnDiagUntilByPlayer)
    , m_respawnDiagNextLogAtByPlayer(respawnDiagNextLogAtByPlayer)
    , m_hooks(std::move(hooks)) {}

void DiagnosticsPhase::RunRespawnDiagnosticsPhase(uint64_t simTicksThisLoop, uint32_t serverTick) {
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
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_mutex, "DiagnosticsPhase::RunRespawnDiagnosticsPhase.resetRespawnedPlayers"
            );
            pipelineResets.reserve(respawnedPlayers.size());
            for (auto &[conn, session] : m_sessions) {
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
            m_hooks.clearChunkPipelineForConnection(conn);
        }
        for (PlayerID playerId : respawnedPlayers) {
            m_respawnDiagUntilByPlayer[playerId] = rbDiagNow + std::chrono::seconds(10);
            m_respawnDiagNextLogAtByPlayer[playerId] = rbDiagNow;
        }
        if (DiagnosticsFlags::g_enableRespawnRubberbandDiagnostics.load(
                std::memory_order_acquire
            )) {
            std::cerr << "[rbdiag/server] respawn reset players=" << respawnedPlayers.size()
                      << " conns=" << pipelineResets.size() << "\n";
        }
    }

    if (DiagnosticsFlags::g_enableRespawnRubberbandDiagnostics.load(std::memory_order_acquire) &&
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
                    auto lk = LockWaitTelemetry::AcquireSessionLock(
                        m_mutex,
                        "DiagnosticsPhase::RunRespawnDiagnosticsPhase.captureRespawnHeartbeat"
                    );
                    for (const auto &[sessionConn, session] : m_sessions) {
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
                                                  ? m_hooks.getChunkSendQueueDepthForClient(conn)
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
