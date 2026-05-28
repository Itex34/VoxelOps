#pragma once

#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

class SimulationPhase {
public:
    struct Hooks {
        std::function<void(double)> updateWorldItems;
        std::function<void(uint32_t)> recordLagCompFrame;
    };

    SimulationPhase(
        PlayerManager &playerManager,
        ChunkManager &chunkManager,
        std::atomic<uint32_t> &serverTick,
        Hooks hooks
    );

    uint64_t RunSimulationPhase(
        double &simAccumulator, uint32_t &serverTick, double &simUs, bool &simBacklog
    );

private:
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
    std::atomic<uint32_t> &m_serverTick;
    Hooks m_hooks;
};
