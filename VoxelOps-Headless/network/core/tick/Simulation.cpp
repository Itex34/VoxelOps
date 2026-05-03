#include "../Runtime.hpp"

namespace {
    constexpr uint32_t kServerTickRateHz = 60u;
} // namespace

uint64_t Runtime::RunSimulationPhase(
    double &simAccumulator, uint32_t &serverTick, double &simUs, bool &simBacklog
) {
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
                                    std::chrono::steady_clock::now() - simStart
    )
                                    .count());
    simBacklog = (simAccumulator >= kServerTickSeconds);
    return simTicksThisLoop;
}
