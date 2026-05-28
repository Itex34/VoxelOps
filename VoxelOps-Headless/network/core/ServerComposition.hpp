#pragma once

#include "../../../Shared/network/PacketType.hpp"
#include "../chunk/ChunkPipelineState.hpp"
#include "../chunk/ChunkStreamingService.hpp"
#include "../connection/BroadcastService.hpp"
#include "../connection/ConnectionService.hpp"
#include "../gameplay/BlockEditExecutionService.hpp"
#include "../gameplay/BlockEditRequestService.hpp"
#include "../gameplay/CombatExecutionService.hpp"
#include "../gameplay/CombatRequestService.hpp"
#include "../gameplay/InventoryActionService.hpp"
#include "../gameplay/MatchService.hpp"
#include "../gameplay/WorldItemService.hpp"
#include "../persistence/AdminControlService.hpp"
#include "../persistence/AdminService.hpp"
#include "../persistence/ChatService.hpp"
#include "../session/SessionLifecycleService.hpp"
#include "../session/SessionState.hpp"
#include "../tick/ChunkInterestPhase.hpp"
#include "../tick/ChunkSendPhase.hpp"
#include "../tick/CollisionPrewarmPhase.hpp"
#include "../tick/DiagnosticsPhase.hpp"
#include "../tick/GameplayPhase.hpp"
#include "../tick/ReplicationPhase.hpp"
#include "../tick/SimulationPhase.hpp"
#include "../tick/TickNetworkPhase.hpp"
#include "ServerControlService.hpp"
#include "ServerTickLoop.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ServerComposition {
public:
    ServerComposition(std::atomic<bool> &quit, HSteamNetPollGroup &pollGroup);

    void ResetRuntimeState();
    void StartBackgroundServices();
    void StopBackgroundServices();
    void ShutdownClientSessions();

    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo);
    void BroadcastRaw(const void *data, uint32_t len, HSteamNetConnection except);

    void SaveHistoryToFile();
    void LoadHistoryFromFile();
    void SaveAdminsToFile();
    void LoadAdminsFromFile();

    bool SetAdminByUsername(const std::string &target, bool isAdmin);
    bool IsAdminUsername(const std::string &usernameOrIdentity);
    std::vector<std::pair<std::string, bool>> GetConnectedUsers();
    std::vector<std::string> GetAdminUsernames();
    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled();

    void RunTickLoop();

private:
    void HandlePlayerInputPacket(
        HSteamNetConnection incoming,
        const void *data,
        uint32_t size,
        uint64_t &playerInputPacketsThisLoop
    );
    void HandleChunkRequestPacket(
        HSteamNetConnection incoming,
        const void *data,
        uint32_t size,
        uint64_t &chunkRequestPacketsThisLoop
    );

    CombatExecutionService::Hooks BuildCombatExecutionHooks();
    BlockEditRequestService::Hooks BuildBlockEditRequestHooks();
    CombatRequestService::Hooks BuildCombatRequestHooks();
    SessionLifecycleService::Hooks BuildSessionLifecycleHooks();
    ConnectionService::Hooks BuildConnectionHooks();
    TickNetworkPhase::Hooks BuildTickNetworkHooks();
    ReplicationPhase::Hooks BuildReplicationHooks();
    ChunkInterestPhase::Hooks BuildChunkInterestHooks();
    ChunkSendPhase::Hooks BuildChunkSendHooks();
    GameplayPhase::Hooks BuildGameplayHooks();
    DiagnosticsPhase::Hooks BuildDiagnosticsHooks();
    SimulationPhase::Hooks BuildSimulationHooks();

private:
    std::atomic<uint32_t> m_serverTick{0};

    SessionState m_sessionState;
    std::unordered_map<PlayerID, bool> m_lastAliveByPlayerId;
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point> m_respawnDiagUntilByPlayer;
    std::unordered_map<PlayerID, std::chrono::steady_clock::time_point> m_respawnDiagNextLogAtByPlayer;
    PlayerManager m_playerManager;
    ChunkManager m_chunkManager;
    WorldItemService m_worldItemService;
    InventoryActionService m_inventoryActionService;
    BlockEditExecutionService m_blockEditExecutionService;
    CombatExecutionService m_combatExecutionService;
    BlockEditRequestService m_blockEditRequestService;
    CombatRequestService m_combatRequestService;
    MatchService m_matchService;

    ChatService m_chatService;
    AdminService m_adminService;
    AdminControlService m_adminControlService;
    ServerControlService m_serverControlService;

    ChunkPipelineState m_chunkPipelineState;
    ChunkStreamingService m_chunkStreamingService;
    BroadcastService m_broadcastService;
    SessionLifecycleService m_sessionLifecycleService;
    ConnectionService m_connectionService;
    TickNetworkPhase m_tickNetworkPhase;
    ReplicationPhase m_replicationPhase;
    ChunkInterestPhase m_chunkInterestPhase;
    ChunkSendPhase m_chunkSendPhase;
    CollisionPrewarmPhase m_collisionPrewarmPhase;
    GameplayPhase m_gameplayPhase;
    DiagnosticsPhase m_diagnosticsPhase;
    SimulationPhase m_simulationPhase;
    ServerTickLoop m_tickLoop;
};
