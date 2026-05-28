#include "SimulationPhase.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

namespace {
constexpr uint32_t kServerTickRateHz = 60u;
} // namespace

SimulationPhase::SimulationPhase(
    PlayerManager &playerManager,
    ChunkManager &chunkManager,
    std::atomic<uint32_t> &serverTick,
    Hooks hooks
)
    : m_playerManager(playerManager)
    , m_chunkManager(chunkManager)
    , m_serverTick(serverTick)
    , m_hooks(std::move(hooks)) {}

uint64_t SimulationPhase::RunSimulationPhase(
    double &simAccumulator, uint32_t &serverTick, double &simUs, bool &simBacklog
) {
    constexpr double kServerTickSeconds = 1.0 / static_cast<double>(kServerTickRateHz);
    constexpr size_t kMaxSimCatchupTicksPerLoop = 4;

    const auto simStart = std::chrono::steady_clock::now();
    uint64_t simTicksThisLoop = 0;
    while (simAccumulator >= kServerTickSeconds && simTicksThisLoop < kMaxSimCatchupTicksPerLoop) {
        m_playerManager.update(kServerTickSeconds, m_chunkManager);
        m_hooks.updateWorldItems(kServerTickSeconds);
        simAccumulator -= kServerTickSeconds;
        ++serverTick;
        m_serverTick.store(serverTick, std::memory_order_release);
        m_hooks.recordLagCompFrame(serverTick);
        ++simTicksThisLoop;
    }
    simUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - simStart
    )
                                    .count());
    simBacklog = (simAccumulator >= kServerTickSeconds);
    return simTicksThisLoop;
}
