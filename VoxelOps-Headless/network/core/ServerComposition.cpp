#include "ServerComposition.hpp"

#include "../../game/combat/CombatFeedback.hpp"
#include "../protocol/PacketParsers.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <utility>

ServerComposition::ServerComposition(std::atomic<bool> &quit, HSteamNetPollGroup &pollGroup)
    : m_adminControlService(
          m_sessionState.MutexRef(), m_sessionState.SessionsRef(), m_playerManager, m_adminService
      )
    , m_serverControlService(m_adminControlService, m_playerManager)
    , m_worldItemService(
          m_sessionState.MutexRef(), m_sessionState.SessionsRef(), m_playerManager, m_chunkManager
      )
    , m_inventoryActionService(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          m_worldItemService
      )
    , m_blockEditExecutionService(
          m_sessionState.MutexRef(), m_sessionState.SessionsRef(), m_playerManager, m_chunkManager
      )
    , m_combatExecutionService(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          m_chunkManager,
          m_serverTick,
          BuildCombatExecutionHooks()
      )
    , m_blockEditRequestService(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          m_worldItemService,
          BuildBlockEditRequestHooks()
      )
    , m_combatRequestService(BuildCombatRequestHooks())
    , m_chunkStreamingService(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_chunkManager,
          m_chunkPipelineState
      )
    , m_broadcastService(m_sessionState.MutexRef(), m_sessionState.SessionsRef())
    , m_sessionLifecycleService(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          BuildSessionLifecycleHooks()
      )
    , m_connectionService(
          m_sessionState,
          m_playerManager,
          m_chunkManager,
          m_chatService,
          m_adminService,
          pollGroup,
          BuildConnectionHooks()
      )
    , m_tickNetworkPhase(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          pollGroup,
          BuildTickNetworkHooks()
      )
    , m_replicationPhase(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          BuildReplicationHooks()
      )
    , m_chunkInterestPhase(
          m_sessionState.MutexRef(), m_sessionState.SessionsRef(), BuildChunkInterestHooks()
      )
    , m_chunkSendPhase(BuildChunkSendHooks())
    , m_collisionPrewarmPhase(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          m_chunkManager
      )
    , m_gameplayPhase(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_matchService.MatchStartTimeRef(),
          m_matchService.MatchDurationRef(),
          m_matchService.MatchStartedRef(),
          m_matchService.MatchEndedRef(),
          m_matchService.MatchWinnerRef(),
          BuildGameplayHooks()
      )
    , m_diagnosticsPhase(
          m_sessionState.MutexRef(),
          m_sessionState.SessionsRef(),
          m_playerManager,
          m_lastAliveByPlayerId,
          m_respawnDiagUntilByPlayer,
          m_respawnDiagNextLogAtByPlayer,
          BuildDiagnosticsHooks()
      )
    , m_simulationPhase(m_playerManager, m_chunkManager, m_serverTick, BuildSimulationHooks())
    , m_tickLoop(
          quit,
          m_tickNetworkPhase,
          m_simulationPhase,
          m_diagnosticsPhase,
          m_replicationPhase,
          m_gameplayPhase,
          m_chunkInterestPhase,
          m_chunkSendPhase,
          m_collisionPrewarmPhase
      ) {}

void ServerComposition::ResetRuntimeState() {
    m_serverTick.store(0, std::memory_order_release);
    m_combatExecutionService.InvalidateCombatSnapshotCache();
    m_worldItemService.Reset();
    m_matchService.ResetForNewRun();
    m_sessionState.ClearSessions();
}

void ServerComposition::StartBackgroundServices() {
    m_chunkStreamingService.StartChunkPipeline();
}

void ServerComposition::StopBackgroundServices() {
    m_chunkStreamingService.StopChunkPipeline();
}

void ServerComposition::ShutdownClientSessions() {
    const auto sessions = m_sessionState.SnapshotSessions();
    m_sessionState.ClearSessions();
    m_matchService.ClearScores();
    m_combatExecutionService.InvalidateCombatSnapshotCache();
    m_worldItemService.Reset();

    for (const auto &[conn, session] : sessions) {
        m_sessionLifecycleService.TeardownClientSession(conn, session, "server shutting down", true);
    }
}

void ServerComposition::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo) {
    m_connectionService.OnConnectionStatusChanged(pInfo);
}

void ServerComposition::BroadcastRaw(const void *data, uint32_t len, HSteamNetConnection except) {
    m_broadcastService.BroadcastRaw(data, len, except);
}

void ServerComposition::SaveHistoryToFile() {
    m_chatService.Save();
}

void ServerComposition::LoadHistoryFromFile() {
    m_chatService.Load();
}

void ServerComposition::SaveAdminsToFile() {
    m_adminService.Save();
}

void ServerComposition::LoadAdminsFromFile() {
    m_adminService.Load();
}

bool ServerComposition::SetAdminByUsername(const std::string &target, bool isAdmin) {
    return m_serverControlService.SetAdminByUsername(target, isAdmin);
}

bool ServerComposition::IsAdminUsername(const std::string &usernameOrIdentity) {
    return m_serverControlService.IsAdminUsername(usernameOrIdentity);
}

std::vector<std::pair<std::string, bool>> ServerComposition::GetConnectedUsers() {
    return m_serverControlService.GetConnectedUsers();
}

std::vector<std::string> ServerComposition::GetAdminUsernames() {
    return m_serverControlService.GetAdminUsernames();
}

void ServerComposition::SetDebugLoggingEnabled(bool enabled) {
    m_serverControlService.SetDebugLoggingEnabled(enabled);
}

bool ServerComposition::IsDebugLoggingEnabled() {
    return m_serverControlService.IsDebugLoggingEnabled();
}

void ServerComposition::RunTickLoop() {
    m_tickLoop.Run();
}

void ServerComposition::HandlePlayerInputPacket(
    HSteamNetConnection incoming,
    const void *data,
    uint32_t size,
    uint64_t &playerInputPacketsThisLoop
) {
    ++playerInputPacketsThisLoop;
    PlayerInput input{};
    if (!NetPacket::ParsePlayerInputPacket(reinterpret_cast<const uint8_t *>(data), size, input)) {
        std::cout << "[recv] malformed PlayerInput (size=" << size << ")\n";
        return;
    }

    std::string username;
    PlayerID playerId = 0;
    m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        const auto it = sessions.find(incoming);
        if (it != sessions.end()) {
            username = it->second.username;
            playerId = it->second.playerId;
        }
    });
    if (!username.empty() && playerId != 0) {
        m_playerManager.enqueuePlayerInput(playerId, input);
        m_playerManager.setEquippedWeapon(playerId, input.weaponId);
    } else {
        std::cout << "[input] unregistered conn = " << incoming << " tick = " << input.inputTick
                  << "\n";
    }
}

void ServerComposition::HandleChunkRequestPacket(
    HSteamNetConnection incoming,
    const void *data,
    uint32_t size,
    uint64_t &chunkRequestPacketsThisLoop
) {
    ++chunkRequestPacketsThisLoop;
    ChunkRequest req{};
    if (!NetPacket::ParseChunkRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cout << "[recv] malformed ChunkRequest (size=" << size << ")\n";
        return;
    }

    const glm::ivec3 centerChunk(req.chunkX, req.chunkY, req.chunkZ);
    const uint16_t clampedViewDistance = ChunkStreamingService::ClampViewDistance(req.viewDistance);
    bool registered = false;
    m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        auto it = sessions.find(incoming);
        if (it != sessions.end() && !it->second.username.empty() && it->second.playerId != 0) {
            const auto now = std::chrono::steady_clock::now();
            const bool hadInterest = it->second.hasChunkInterest;
            const bool centerChanged = !hadInterest ||
                                       it->second.interestCenterChunk.x != centerChunk.x ||
                                       it->second.interestCenterChunk.y != centerChunk.y ||
                                       it->second.interestCenterChunk.z != centerChunk.z;
            const bool viewChanged = !hadInterest || it->second.viewDistance != clampedViewDistance;

            it->second.interestCenterChunk = centerChunk;
            it->second.viewDistance = clampedViewDistance;
            it->second.hasChunkInterest = true;

            if (centerChanged || viewChanged || now >= it->second.nextChunkInterestUpdateAt) {
                it->second.chunkInterestDirty = true;
                it->second.nextChunkInterestUpdateAt = std::chrono::steady_clock::time_point::min();
            }

            registered = true;
        }
    });
    if (!registered) {
        return;
    }
}

CombatExecutionService::Hooks ServerComposition::BuildCombatExecutionHooks() {
    return CombatExecutionService::Hooks{
        [this](PlayerID killerId, std::string_view killerUsername, PlayerID victimId, uint16_t weaponId) {
            m_matchService.ApplyKillScore(killerId, victimId);

            std::string victimUsername;
            m_sessionState.WithLock([&](ClientSessionManager &sessions) {
                for (const auto &[_, clientSession] : sessions) {
                    if (clientSession.playerId == victimId) {
                        victimUsername = clientSession.username;
                        break;
                    }
                }
            });
            if (victimUsername.empty()) {
                victimUsername = CombatFeedback::FallbackVictimUsername(victimId);
            }
            const std::string killfeedPacket = CombatFeedback::BuildKillfeedPacket(
                std::string(killerUsername), victimUsername, weaponId
            );
            m_broadcastService.BroadcastRaw(
                killfeedPacket.data(),
                static_cast<uint32_t>(killfeedPacket.size()),
                k_HSteamNetConnection_Invalid
            );
        },
    };
}

BlockEditRequestService::Hooks ServerComposition::BuildBlockEditRequestHooks() {
    return BlockEditRequestService::Hooks{
        [this](PlayerID requesterId, const BlockPlaceRequest &request) {
            return m_blockEditExecutionService.ExecuteBlockPlaceRequest(requesterId, request);
        },
        [this](PlayerID requesterId, const BlockBreakRequest &request) {
            return m_blockEditExecutionService.ExecuteBlockBreakRequest(requesterId, request);
        },
    };
}

CombatRequestService::Hooks ServerComposition::BuildCombatRequestHooks() {
    return CombatRequestService::Hooks{
        [this](HSteamNetConnection incoming, const ShootRequest &req) {
            return m_combatExecutionService.ExecuteShootRequest(incoming, req);
        },
        [this](HSteamNetConnection incoming, const GrappleRequest &req) {
            return m_combatExecutionService.ExecuteGrappleRequest(incoming, req);
        },
    };
}

SessionLifecycleService::Hooks ServerComposition::BuildSessionLifecycleHooks() {
    return SessionLifecycleService::Hooks{
        [this](HSteamNetConnection conn) { m_chunkStreamingService.ClearChunkPipelineForConnection(conn); },
        [this](PlayerID playerId) { m_matchService.OnPlayerDetached(playerId); },
        [this]() { m_combatExecutionService.InvalidateCombatSnapshotCache(); },
        [this](const void *data, uint32_t len, HSteamNetConnection except) {
            m_broadcastService.BroadcastRaw(data, len, except);
        },
    };
}

ConnectionService::Hooks ServerComposition::BuildConnectionHooks() {
    return ConnectionService::Hooks{
        [this](std::string_view identity, std::string_view requestedName, HSteamNetConnection incomingConn) {
            return m_chunkStreamingService.BuildDisplayNameForIdentityLocked(
                identity, requestedName, incomingConn
            );
        },
        [this]() { m_combatExecutionService.InvalidateCombatSnapshotCache(); },
        [this](PlayerID playerId, size_t activePlayers) {
            m_matchService.OnSessionAttached(playerId, activePlayers);
        },
        [this](const void *data, uint32_t len, HSteamNetConnection except) {
            m_broadcastService.BroadcastRaw(data, len, except);
        },
        [this](const ClientSessionManager::ChunkCoord &coord) {
            return m_chunkStreamingService.PrepareChunkForStreaming(coord);
        },
        [this](HSteamNetConnection conn, const ClientSessionManager::ChunkCoord &coord) {
            return m_chunkStreamingService.SendChunkData(conn, coord);
        },
        [this](HSteamNetConnection conn, const ClientSessionManager::ChunkCoord &coord) {
            return m_chunkStreamingService.QueueChunkPreparation(conn, coord);
        },
        [this](HSteamNetConnection conn,
               const ClientSessionManager::ClientSession &session,
               const char *closeReason,
               bool closeConnection) {
            m_sessionLifecycleService.TeardownClientSession(conn, session, closeReason, closeConnection);
        },
    };
}

TickNetworkPhase::Hooks ServerComposition::BuildTickNetworkHooks() {
    return TickNetworkPhase::Hooks{
        [this](HSteamNetConnection incoming, PacketType packetType, uint32_t bytes) {
            return m_connectionService.IsInboundRateLimitExceeded(incoming, packetType, bytes);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_connectionService.HandleConnectRequest(incoming, data, size);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_connectionService.HandleMessagePacket(incoming, data, size);
        },
        [this](HSteamNetConnection incoming,
               const void *data,
               uint32_t size,
               uint64_t &playerInputPacketsThisLoop) {
            HandlePlayerInputPacket(incoming, data, size, playerInputPacketsThisLoop);
        },
        [this](HSteamNetConnection incoming,
               const void *data,
               uint32_t size,
               uint64_t &chunkRequestPacketsThisLoop) {
            HandleChunkRequestPacket(incoming, data, size, chunkRequestPacketsThisLoop);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_blockEditRequestService.HandleBlockPlaceRequestPacket(incoming, data, size);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_blockEditRequestService.HandleBlockBreakRequestPacket(incoming, data, size);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_combatRequestService.HandleShootRequestPacket(incoming, data, size);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_combatRequestService.HandleGrappleRequestPacket(incoming, data, size);
        },
        [this](HSteamNetConnection incoming, const void *data, uint32_t size) {
            m_inventoryActionService.HandleInventoryActionRequestPacket(incoming, data, size);
        },
        [this](HSteamNetConnection conn,
               const ClientSessionManager::ClientSession &session,
               const char *closeReason,
               bool closeConnection) {
            m_sessionLifecycleService.TeardownClientSession(conn, session, closeReason, closeConnection);
        },
    };
}

ReplicationPhase::Hooks ServerComposition::BuildReplicationHooks() {
    return ReplicationPhase::Hooks{
        [this](const std::vector<std::pair<HSteamNetConnection, PlayerID>> &recipients, uint32_t serverTick) {
            m_worldItemService.SendWorldItemSnapshots(recipients, serverTick);
        },
        [this](HSteamNetConnection conn,
               const ClientSessionManager::ClientSession &session,
               const char *closeReason,
               bool closeConnection) {
            m_sessionLifecycleService.TeardownClientSession(conn, session, closeReason, closeConnection);
        },
    };
}

ChunkInterestPhase::Hooks ServerComposition::BuildChunkInterestHooks() {
    return ChunkInterestPhase::Hooks{
        [this](HSteamNetConnection conn, const glm::ivec3 &centerChunk, uint16_t viewDistance) {
            m_chunkStreamingService.UpdateChunkStreamingForClient(conn, centerChunk, viewDistance);
        },
    };
}

ChunkSendPhase::Hooks ServerComposition::BuildChunkSendHooks() {
    return ChunkSendPhase::Hooks{
        [this](size_t globalBudget, size_t perClientBudget) {
            return m_chunkStreamingService.FlushChunkSendQueues(globalBudget, perClientBudget);
        },
    };
}

GameplayPhase::Hooks ServerComposition::BuildGameplayHooks() {
    return GameplayPhase::Hooks{
        [this](PlayerID playerId, uint32_t &kills, uint32_t &deaths) {
            return m_matchService.GetPlayerScore(playerId, kills, deaths);
        },
        [this](const void *data, uint32_t len, HSteamNetConnection except) {
            m_broadcastService.BroadcastRaw(data, len, except);
        },
    };
}

DiagnosticsPhase::Hooks ServerComposition::BuildDiagnosticsHooks() {
    return DiagnosticsPhase::Hooks{
        [this](HSteamNetConnection conn) { m_chunkStreamingService.ClearChunkPipelineForConnection(conn); },
        [this](HSteamNetConnection conn) {
            return m_chunkStreamingService.GetChunkSendQueueDepthForClient(conn);
        },
    };
}

SimulationPhase::Hooks ServerComposition::BuildSimulationHooks() {
    return SimulationPhase::Hooks{
        [this](double dt) { m_worldItemService.UpdateWorldItems(dt); },
        [this](uint32_t serverTick) { m_combatExecutionService.RecordLagCompFrame(serverTick); },
    };
}
