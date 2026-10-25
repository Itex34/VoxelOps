#include "SimulationPhase.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

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
    double &simAccumulator,
    uint32_t &serverTick,
    double &simUs,
    bool &simBacklog,
    std::vector<TickTimingSample> &tickTimings
) {
    constexpr double kServerTickSeconds = 1.0 / static_cast<double>(kTickRateHz);
    constexpr size_t kMaxSimCatchupTicksPerLoop = 4;

    const auto simStart = std::chrono::steady_clock::now();
    uint64_t simTicksThisLoop = 0;
    tickTimings.clear();
    while (simAccumulator >= kServerTickSeconds && simTicksThisLoop < kMaxSimCatchupTicksPerLoop) {
        const auto tickStart = std::chrono::steady_clock::now();
        const double intervalMs =
            (m_lastTickStart.time_since_epoch().count() == 0)
                ? 0.0
                : std::chrono::duration<double, std::milli>(tickStart - m_lastTickStart).count();
        m_lastTickStart = tickStart;

        m_playerManager.update(kServerTickSeconds, m_chunkManager);
        m_hooks.updateWorldItems(kServerTickSeconds);
        simAccumulator -= kServerTickSeconds;
        ++serverTick;
        m_serverTick.store(serverTick, std::memory_order_release);
        m_playerManager.CaptureReplicationSnapshot(serverTick);
        m_hooks.recordLagCompFrame(serverTick);
        const auto tickEnd = std::chrono::steady_clock::now();
        tickTimings.push_back(TickTimingSample{
            std::chrono::duration<double, std::milli>(tickEnd - tickStart).count(), intervalMs
        });
        ++simTicksThisLoop;
    }
    simUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - simStart
    )
                                    .count());
    simBacklog = (simAccumulator >= kServerTickSeconds);
    return simTicksThisLoop;
}
