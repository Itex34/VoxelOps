#pragma once

#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class SimulationPhase {
public:
    static constexpr uint32_t kTickRateHz = 60u;

    struct TickTimingSample {
        double workMs = 0.0;
        double intervalMs = 0.0;
    };

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
        double &simAccumulator,
        uint32_t &serverTick,
        double &simUs,
        bool &simBacklog,
        std::vector<TickTimingSample> &tickTimings
    );

private:
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
    std::atomic<uint32_t> &m_serverTick;
    Hooks m_hooks;
    std::chrono::steady_clock::time_point m_lastTickStart{};
};
