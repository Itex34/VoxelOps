Target structure:

server/
├── Runtime.hpp / Runtime.cpp
│
├── net/
│   ├── NetworkServer.hpp / .cpp
│   ├── PacketDispatcher.hpp / .cpp
│   └── PacketReader.hpp / .cpp
│
├── session/
│   ├── ClientSession.hpp
│   ├── ClientSessionManager.hpp / .cpp
│   └── RateLimitState.hpp
│
├── world/
│   ├── ChunkStreamingService.hpp / .cpp
│   ├── ChunkTypes.hpp
│   ├── BlockEditService.hpp / .cpp
│   └── WorldItemService.hpp / .cpp
│
├── gameplay/
│   ├── MatchManager.hpp / .cpp
│   ├── CombatService.hpp / .cpp
│   ├── InventoryService.hpp / .cpp
│   └── RespawnDiagnostics.hpp / .cpp
│
├── persistence/
│   ├── ChatService.hpp / .cpp
│   └── AdminService.hpp / .cpp
│
└── loop/
    ├── ServerLoop.hpp / .cpp
    └── LoopStats.hpp

Your new Runtime should become small:

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool Start(uint16_t port = 27015);
    void Run();
    void Stop();

    bool SetAdminByUsername(const std::string& username, bool isAdmin);
    bool IsAdminUsername(const std::string& username);
    std::vector<std::pair<std::string, bool>> GetConnectedUsers();
    std::vector<std::string> GetAdminUsernames();

    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled();

    void BroadcastRaw(
        const void* data,
        uint32_t len,
        HSteamNetConnection except = k_HSteamNetConnection_Invalid
    );

    static Runtime* s_instance;

private:
    void MainLoop();
    void Shutdown();

    static void SteamNetConnectionStatusChangedCallback(
        SteamNetConnectionStatusChangedCallback_t* pInfo
    );

    void OnConnectionStatusChanged(
        SteamNetConnectionStatusChangedCallback_t* pInfo
    );

private:
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_started{false};
    std::atomic<uint32_t> m_serverTick{0};

    std::mutex m_shutdownMutex;
    bool m_shutdownComplete = false;

    NetworkServer m_network;
    ClientSessionManager m_sessions;
    PacketDispatcher m_packets;

    PlayerManager m_playerManager;
    ChunkManager m_chunkManager;

    ChunkStreamingService m_chunkStreaming;
    BlockEditService m_blockEdits;
    WorldItemService m_worldItems;
    CombatService m_combat;
    InventoryService m_inventory;
    MatchManager m_match;
    RespawnDiagnostics m_respawnDiagnostics;

    ChatService m_chat;
    AdminService m_admins;

    bool m_debugLoggingEnabled = false;
};

Main ownership split:

NetworkServer

Owns:

HSteamNetPollGroup m_pollGroup;
HSteamListenSocket m_listenSock;
BroadcastRaw()
Start()
Shutdown()
PollIncomingMessages()
CloseConnection()
ClientSessionManager

Owns:

std::unordered_map<HSteamNetConnection, ClientSession> m_clients;
std::unordered_map<PlayerID, HSteamNetConnection> m_connectionByPlayerId;
uint32_t m_nextAutoUsername;

Methods:

ClientSession* Find(HSteamNetConnection conn);
ClientSession* FindByPlayerId(PlayerID id);
bool TryGetPlayerId(HSteamNetConnection conn, PlayerID& out);
void Add(HSteamNetConnection conn, ClientSession session);
void Remove(HSteamNetConnection conn);
std::vector<std::pair<std::string, bool>> GetConnectedUsers();
std::string AllocateAutoUsername(HSteamNetConnection conn);
std::string BuildDisplayName(...);
ClientSession.hpp
struct ClientSession {
    std::string identity;
    std::string username;
    PlayerID playerId = 0;
    bool isAdmin = false;

    ChunkInterestState chunkInterest;
    InboundRateLimitState inboundRateLimit;
    ShootRateLimitState shootRateLimit;
};
ChunkStreamingService

Owns:

ChunkCoord
ChunkCoordHash
ChunkPipelineKey
ChunkPipelineKeyHash
ChunkPrepTask

m_chunkPrepQuit
m_chunkPrepThread
m_chunkPipelineMutex
m_chunkPrepCv
m_chunkPrepQueue
m_chunkPrepQueued
m_chunkSendQueues
m_chunkSendQueued

Methods:

Start();
Stop();
UpdateClientStreaming(...);
QueueChunkPreparation(...);
FlushSendQueues(...);
ClearConnection(...);
PruneClientPipeline(...);
SendChunkData(...);
SendChunkUnload(...);
PrepareChunkForStreaming(...);
PacketDispatcher

Owns packet routing, not game state.

Methods:

DispatchInboundPacket(...);
HandleConnectRequest(...);
HandleMessagePacket(...);
HandlePlayerInputPacket(...);
HandleChunkRequestPacket(...);
HandleBlockPlaceRequestPacket(...);
HandleBlockBreakRequestPacket(...);
HandleShootRequestPacket(...);
HandleInventoryActionRequestPacket(...);

It calls other services:

m_combat.ExecuteShootRequest(...)
m_blockEdits.ExecuteBlockPlaceRequest(...)
m_inventory.HandleInventoryAction(...)
m_chat.AddMessage(...)
m_chunkStreaming.UpdateClientStreaming(...)
CombatService

Owns:

std::deque<LagCompensation::LagCompFrame> m_lagCompFrames;
std::vector<ServerPlayerCombatSnapshot> m_combatSnapshotsAliveCache;
uint32_t m_combatSnapshotsAliveCacheTick;
bool m_hasCombatSnapshotsAliveCache;

Methods:

ExecuteShootRequest(...);
RecordLagCompFrame(...);
InvalidateSnapshotCache();
GetCombatSnapshotsForTick(...);
WorldItemService

Owns:

std::unordered_map<uint64_t, WorldItemEntity> m_worldItems;
uint64_t m_nextWorldItemId = 1;

Methods:

SpawnDroppedItem(...);
Update(double deltaSeconds);
SendSnapshots(...);
TryPickupItems(...);
MatchManager

Owns:

std::unordered_map<PlayerID, MatchScore> m_matchScores;
std::chrono::steady_clock::time_point m_matchStartTime;
std::chrono::seconds m_matchDuration{600};
bool m_matchStarted = false;
bool m_matchEnded = false;
std::string m_matchWinner;

Methods:

StartMatch();
UpdateMatch();
AddKill(...);
AddDeath(...);
BuildScoreboardPacket();
ShouldBroadcastScoreboard();
ChatService

Owns:

std::vector<std::pair<std::string, std::string>> m_messageHistory;
static constexpr std::string_view HistoryFile = "chat_history.txt";

Methods:

AddMessage(username, message);
Save();
Load();
GetHistory();
AdminService

Owns:

std::unordered_set<std::string> m_adminIdentities;
static constexpr std::string_view AdminsFile = "admins.txt";

Methods:

Load();
Save();
SetAdmin(identity, bool);
IsAdmin(identity);
GetAdminUsernames(...);

Refactor order I’d use:

1. Move ChunkCoord / ChunkCoordHash into ChunkTypes.hpp
2. Extract ChatService
3. Extract AdminService
4. Extract ClientSession.hpp
5. Extract ChunkStreamingService
6. Extract WorldItemService
7. Extract MatchManager
8. Extract CombatService
9. Extract PacketDispatcher
10. Extract NetworkServer





I’d turn runtime into this:

network/
│   Network.hpp / Network.cpp
│
├── core/
│   Runtime.hpp / Runtime.cpp
│   DiagnosticsFlags.hpp / .cpp
│
├── runtime/
│   Lifecycle.hpp / .cpp
│   Callbacks.hpp / .cpp
│   ServerLoop.hpp / .cpp
│
├── session/
│   ClientSession.hpp
│   ClientSessionManager.hpp / .cpp
│
├── tick/
│   TickPipeline.hpp / .cpp
│   SimulationTick.hpp / .cpp
│   ReplicationTick.hpp / .cpp
│   DiagnosticsTick.hpp / .cpp
│
├── gameplay/
│   BlockEditService.hpp / .cpp
│   CombatService.hpp / .cpp
│   GameplayService.hpp / .cpp
│   InputBuffer.hpp / .cpp
│   Rules.hpp / .cpp
│
├── protocol/
│   PacketDispatcher.hpp / .cpp
│   PacketParsers.hpp / .cpp
│   PacketReader.hpp / .cpp
│   PacketValidation.hpp / .cpp
│   Validation.hpp / .cpp
│
├── replication/
│   ChunkPipeline.hpp / .cpp
│   ChunkStreamingService.hpp / .cpp
│   ChunkStore.hpp / .cpp
│   CompressChunk.hpp / .cpp
│
└── snapshots/
    PlayerSnapshots.hpp / .cpp
    PlayerSnapshotSerializer.hpp / .cpp

The important move is this:

core/runtime/*.cpp
core/tick/*.cpp
gameplay/BlockEdits.cpp
gameplay/Combat.cpp
replication/ChunkStreaming.cpp
replication/ChunkPipeline.cpp

should stop being “extra Runtime.cpp files” and become actual classes.

For example:

replication/ChunkStreaming.cpp
replication/ChunkPipeline.cpp

become:

class ChunkStreamingService;
class ChunkPipeline;
gameplay/Combat.cpp

becomes:

class CombatService;
gameplay/BlockEdits.cpp

becomes:

class BlockEditService;
core/tick/*.cpp

becomes either one class:

class ServerTickPipeline;

or several small phase classes.

Your final Runtime.hpp should look closer to this:

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool Start(uint16_t port = 27015);
    void Run();
    void Stop();

    void BroadcastRaw(
        const void* data,
        uint32_t len,
        HSteamNetConnection except = k_HSteamNetConnection_Invalid
    );

    bool SetAdminByUsername(const std::string& username, bool isAdmin);
    bool IsAdminUsername(const std::string& username);
    std::vector<std::pair<std::string, bool>> GetConnectedUsers();
    std::vector<std::string> GetAdminUsernames();

    static Runtime* s_instance;

private:
    void MainLoop();
    void ShutdownNetworking();

    static void SteamNetConnectionStatusChangedCallback(
        SteamNetConnectionStatusChangedCallback_t* pInfo
    );

    void OnConnectionStatusChanged(
        SteamNetConnectionStatusChangedCallback_t* pInfo
    );

private:
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_started{false};
    std::atomic<uint32_t> m_serverTick{0};

    NetworkServer m_network;
    ClientSessionManager m_sessions;
    PacketDispatcher m_packets;
    ServerTickPipeline m_tickPipeline;

    PlayerManager m_playerManager;
    ChunkManager m_chunkManager;

    AdminService m_admins;
    ChatService m_chat;
    MatchManager m_match;
    CombatService m_combat;
    BlockEditService m_blockEdits;
    ChunkStreamingService m_chunkStreaming;
    WorldItemService m_worldItems;
};

I’d rename/move your current files like this:

core/runtime/Persistence.cpp

split into:

persistence/ChatService.cpp
persistence/AdminService.cpp
core/tick/Pipeline.cpp

becomes:

tick/ServerTickPipeline.cpp
core/tick/Simulation.cpp

becomes methods inside:

tick/SimulationTick.cpp

or:

ServerTickPipeline::RunSimulationPhase()
core/tick/Replication.cpp

becomes:

tick/ReplicationTick.cpp
replication/ChunkStreaming.cpp

becomes:

replication/ChunkStreamingService.cpp
replication/ChunkPipeline.cpp

becomes:

replication/ChunkPipeline.cpp/.hpp
gameplay/Combat.cpp

becomes:

gameplay/CombatService.cpp/.hpp
gameplay/BlockEdits.cpp

becomes:

gameplay/BlockEditService.cpp/.hpp