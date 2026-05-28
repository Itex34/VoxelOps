#pragma once

#include "../tick/ChunkInterestPhase.hpp"
#include "../tick/ChunkSendPhase.hpp"
#include "../tick/CollisionPrewarmPhase.hpp"
#include "../tick/DiagnosticsPhase.hpp"
#include "../tick/GameplayPhase.hpp"
#include "../tick/ReplicationPhase.hpp"
#include "../tick/SimulationPhase.hpp"
#include "../tick/TickNetworkPhase.hpp"

#include <atomic>

class ServerTickLoop {
public:
    ServerTickLoop(
        std::atomic<bool> &quit,
        TickNetworkPhase &tickNetworkPhase,
        SimulationPhase &simulationPhase,
        DiagnosticsPhase &diagnosticsPhase,
        ReplicationPhase &replicationPhase,
        GameplayPhase &gameplayPhase,
        ChunkInterestPhase &chunkInterestPhase,
        ChunkSendPhase &chunkSendPhase,
        CollisionPrewarmPhase &collisionPrewarmPhase
    );

    void Run();

private:
    void ApplyLoopPacing(bool simBacklog);

private:
    std::atomic<bool> &m_quit;
    TickNetworkPhase &m_tickNetworkPhase;
    SimulationPhase &m_simulationPhase;
    DiagnosticsPhase &m_diagnosticsPhase;
    ReplicationPhase &m_replicationPhase;
    GameplayPhase &m_gameplayPhase;
    ChunkInterestPhase &m_chunkInterestPhase;
    ChunkSendPhase &m_chunkSendPhase;
    CollisionPrewarmPhase &m_collisionPrewarmPhase;
};
