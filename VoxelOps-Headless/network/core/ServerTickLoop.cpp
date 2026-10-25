#include "ServerTickLoop.hpp"

#include "DiagnosticsFlags.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>

namespace {
constexpr auto kServerPerfLogInterval = std::chrono::seconds(1);
constexpr double kSlowServerLoopWarnUs = 12000.0;
constexpr double kSlowServerSimWarnUs = 5000.0;
constexpr auto kTickPerfLogInterval = std::chrono::seconds(60);

double Percentile(const std::vector<double> &sortedValues, double percentile) {
    if (sortedValues.empty()) {
        return 0.0;
    }
    const size_t index = static_cast<size_t>(
        std::ceil(percentile * static_cast<double>(sortedValues.size())) - 1.0
    );
    return sortedValues[std::min(index, sortedValues.size() - 1)];
}

struct FrameTimings {
    double messageDrainUs = 0.0;
    double connectionCleanupUs = 0.0;
    double simUs = 0.0;
    double diagnosticsUs = 0.0;
    double snapshotUs = 0.0;
    double scoreboardUs = 0.0;
    double chunkInterestUs = 0.0;
    double chunkSendUs = 0.0;
    double collisionPrewarmUs = 0.0;
};

struct LoopTimingSample {
    double loopWorkMs = 0.0;
    double loopWaitMs = 0.0;
    double loopIntervalMs = 0.0;
    FrameTimings phases{};
};
} // namespace

ServerTickLoop::ServerTickLoop(
    std::atomic<bool> &quit,
    TickNetworkPhase &tickNetworkPhase,
    SimulationPhase &simulationPhase,
    DiagnosticsPhase &diagnosticsPhase,
    ReplicationPhase &replicationPhase,
    GameplayPhase &gameplayPhase,
    ChunkInterestPhase &chunkInterestPhase,
    ChunkSendPhase &chunkSendPhase,
    CollisionPrewarmPhase &collisionPrewarmPhase
)
    : m_quit(quit)
    , m_tickNetworkPhase(tickNetworkPhase)
    , m_simulationPhase(simulationPhase)
    , m_diagnosticsPhase(diagnosticsPhase)
    , m_replicationPhase(replicationPhase)
    , m_gameplayPhase(gameplayPhase)
    , m_chunkInterestPhase(chunkInterestPhase)
    , m_chunkSendPhase(chunkSendPhase)
    , m_collisionPrewarmPhase(collisionPrewarmPhase) {}

void ServerTickLoop::ApplyLoopPacing(bool simBacklog) {
    if (simBacklog) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ServerTickLoop::Run() {
    std::ofstream tickPerfLog("server_tick_perf.txt", std::ios::app);
    if (!tickPerfLog) {
        std::cerr << "[perf/server] unable to open server_tick_perf.txt for writing\n";
    } else {
        const auto runStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()
        )
                                    .count();
        tickPerfLog << "# run_start_unix_ms=" << runStartMs
                    << " configured_tick_hz=" << SimulationPhase::kTickRateHz << "\n"
                    << "window_sec,sim_ticks,actual_tick_hz,tick_work_p50_ms,tick_work_p95_ms,"
                       "tick_work_p99_ms,tick_work_max_ms,tick_interval_p50_ms,"
                       "tick_interval_p95_ms,tick_interval_p99_ms,tick_interval_max_ms,"
                       "backlog_loops,loops,backlog_percent,max_backlog_ms,"
                       "loop_work_p50_ms,loop_work_p95_ms,loop_work_p99_ms,loop_work_max_ms,"
                       "loop_wait_p50_ms,loop_wait_p95_ms,loop_wait_p99_ms,loop_wait_max_ms,"
                       "loop_interval_p50_ms,loop_interval_p95_ms,loop_interval_p99_ms,"
                       "loop_interval_max_ms,message_drain_p50_ms,message_drain_p95_ms,"
                       "message_drain_p99_ms,message_drain_max_ms,connection_cleanup_p50_ms,"
                       "connection_cleanup_p95_ms,connection_cleanup_p99_ms,"
                       "connection_cleanup_max_ms,prewarm_p50_ms,prewarm_p95_ms,prewarm_p99_ms,"
                       "prewarm_max_ms,simulation_batch_p50_ms,simulation_batch_p95_ms,"
                       "simulation_batch_p99_ms,simulation_batch_max_ms,diagnostics_p50_ms,"
                       "diagnostics_p95_ms,diagnostics_p99_ms,diagnostics_max_ms,snapshot_p50_ms,"
                       "snapshot_p95_ms,snapshot_p99_ms,snapshot_max_ms,scoreboard_p50_ms,"
                       "scoreboard_p95_ms,scoreboard_p99_ms,scoreboard_max_ms,"
                       "chunk_interest_p50_ms,chunk_interest_p95_ms,chunk_interest_p99_ms,"
                       "chunk_interest_max_ms,chunk_send_p50_ms,chunk_send_p95_ms,"
                       "chunk_send_p99_ms,chunk_send_max_ms\n";
        tickPerfLog.flush();
    }

    auto lastFrameTime = std::chrono::steady_clock::now();
    auto lastSnapshotTime = lastFrameTime;
    constexpr uint32_t kSnapshotSendRateHz = 60u;
    constexpr double kSnapshotSendSeconds = 1.0 / static_cast<double>(kSnapshotSendRateHz);
    const auto snapshotInterval = std::chrono::duration<double>(kSnapshotSendSeconds);

    double simAccumulator = 0.0;
    uint32_t serverTick = 0;
    auto nextChunkSendFlushAt = std::chrono::steady_clock::now();
    auto nextCollisionPrewarmAt = std::chrono::steady_clock::now();
    auto perfWindowStart = std::chrono::steady_clock::now();
    auto tickPerfWindowStart = perfWindowStart;
    auto nextScoreboardBroadcastAt = std::chrono::steady_clock::now();

    std::vector<SimulationPhase::TickTimingSample> tickTimingsThisLoop;
    tickTimingsThisLoop.reserve(4);
    std::vector<double> tickWorkSamplesMs;
    std::vector<double> tickIntervalSamplesMs;
    std::vector<LoopTimingSample> loopTimingSamples;
    tickWorkSamplesMs.reserve(SimulationPhase::kTickRateHz * 60u);
    tickIntervalSamplesMs.reserve(SimulationPhase::kTickRateHz * 60u);
    loopTimingSamples.reserve(4096);
    uint64_t tickPerfLoops = 0;
    uint64_t tickPerfBacklogLoops = 0;
    double tickPerfMaxBacklogMs = 0.0;

    const auto writeTickPerfWindow = [&](std::chrono::steady_clock::time_point windowEnd) {
        if (!tickPerfLog || tickPerfLoops == 0) {
            return;
        }
        const double windowSeconds =
            std::chrono::duration<double>(windowEnd - tickPerfWindowStart).count();
        std::sort(tickWorkSamplesMs.begin(), tickWorkSamplesMs.end());
        std::sort(tickIntervalSamplesMs.begin(), tickIntervalSamplesMs.end());
        const double actualTickHz =
            (windowSeconds > 0.0) ? tickWorkSamplesMs.size() / windowSeconds : 0.0;
        const double backlogPercent =
            100.0 * static_cast<double>(tickPerfBacklogLoops) / tickPerfLoops;
        const auto appendLoopPercentiles = [&](auto selectValue) {
            std::vector<double> samples;
            samples.reserve(loopTimingSamples.size());
            for (const LoopTimingSample &sample : loopTimingSamples) {
                samples.push_back(selectValue(sample));
            }
            std::sort(samples.begin(), samples.end());
            tickPerfLog << ',' << Percentile(samples, 0.50) << ','
                        << Percentile(samples, 0.95) << ','
                        << Percentile(samples, 0.99) << ','
                        << (samples.empty() ? 0.0 : samples.back());
        };
        tickPerfLog << std::fixed << std::setprecision(3) << windowSeconds << ','
                    << tickWorkSamplesMs.size() << ',' << actualTickHz << ','
                    << Percentile(tickWorkSamplesMs, 0.50) << ','
                    << Percentile(tickWorkSamplesMs, 0.95) << ','
                    << Percentile(tickWorkSamplesMs, 0.99) << ','
                    << (tickWorkSamplesMs.empty() ? 0.0 : tickWorkSamplesMs.back()) << ','
                    << Percentile(tickIntervalSamplesMs, 0.50) << ','
                    << Percentile(tickIntervalSamplesMs, 0.95) << ','
                    << Percentile(tickIntervalSamplesMs, 0.99) << ','
                    << (tickIntervalSamplesMs.empty() ? 0.0 : tickIntervalSamplesMs.back()) << ','
                    << tickPerfBacklogLoops << ',' << tickPerfLoops << ',' << backlogPercent << ','
                    << tickPerfMaxBacklogMs;
        appendLoopPercentiles([](const LoopTimingSample &s) { return s.loopWorkMs; });
        appendLoopPercentiles([](const LoopTimingSample &s) { return s.loopWaitMs; });
        appendLoopPercentiles([](const LoopTimingSample &s) { return s.loopIntervalMs; });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.messageDrainUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.connectionCleanupUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.collisionPrewarmUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) { return s.phases.simUs / 1000.0; });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.diagnosticsUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.snapshotUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.scoreboardUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.chunkInterestUs / 1000.0;
        });
        appendLoopPercentiles([](const LoopTimingSample &s) {
            return s.phases.chunkSendUs / 1000.0;
        });
        tickPerfLog << '\n';
        tickPerfLog.flush();
        tickWorkSamplesMs.clear();
        tickIntervalSamplesMs.clear();
        loopTimingSamples.clear();
        tickPerfLoops = 0;
        tickPerfBacklogLoops = 0;
        tickPerfMaxBacklogMs = 0.0;
        tickPerfWindowStart = windowEnd;
    };

    uint64_t perfLoops = 0;
    uint64_t perfMessages = 0;
    uint64_t perfPlayerInputs = 0;
    uint64_t perfChunkRequests = 0;
    uint64_t perfSimTicks = 0;
    uint64_t perfCollisionPrewarmGenerated = 0;
    uint64_t perfChunkInterestTasks = 0;
    uint64_t perfChunksSent = 0;
    uint64_t perfScoreboardBroadcasts = 0;
    double perfLoopUsTotal = 0.0;
    double perfLoopUsMax = 0.0;
    double perfMessageDrainUsTotal = 0.0;
    double perfSimUsTotal = 0.0;
    double perfCollisionPrewarmUsTotal = 0.0;
    double perfSnapshotUsTotal = 0.0;
    double perfChunkInterestUsTotal = 0.0;
    double perfChunkSendUsTotal = 0.0;
    auto lastLoopStart = std::chrono::steady_clock::time_point{};
    auto lastLoopWorkEnd = std::chrono::steady_clock::time_point{};

    while (!m_quit) {
        const auto loopStart = std::chrono::steady_clock::now();
        const double loopIntervalMs =
            (lastLoopStart.time_since_epoch().count() == 0)
                ? 0.0
                : std::chrono::duration<double, std::milli>(loopStart - lastLoopStart).count();
        const double loopWaitMs =
            (lastLoopWorkEnd.time_since_epoch().count() == 0)
                ? 0.0
                : std::chrono::duration<double, std::milli>(loopStart - lastLoopWorkEnd).count();
        lastLoopStart = loopStart;

        uint64_t msgPacketsThisLoop = 0;
        uint64_t playerInputPacketsThisLoop = 0;
        uint64_t chunkRequestPacketsThisLoop = 0;
        FrameTimings frameTimings;
        bool simBacklog = false;

        const auto frameNow = std::chrono::steady_clock::now();
        double deltaSeconds = std::chrono::duration<double>(frameNow - lastFrameTime).count();
        if (deltaSeconds > 0.25) {
            deltaSeconds = 0.25;
        }
        lastFrameTime = frameNow;
        simAccumulator += deltaSeconds;

        m_tickNetworkPhase.RunInboundMessagePhase(
            msgPacketsThisLoop,
            playerInputPacketsThisLoop,
            chunkRequestPacketsThisLoop,
            frameTimings.messageDrainUs
        );
        auto phaseStart = std::chrono::steady_clock::now();
        m_tickNetworkPhase.RunConnectionCleanupPhase();
        frameTimings.connectionCleanupUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - phaseStart
            )
                .count()
        );
        // Prewarm runs BEFORE simulation so that neighboring chunks are available
        // during collision resolution. Running it after causes a guaranteed 1-tick
        // divergence every time a player's AABB first crosses into a new chunk,
        // because the server sees the neighbor as missing (solid) while the client
        // may have already received it from streaming.
        const size_t collisionPrewarmGeneratedThisLoop =
            m_collisionPrewarmPhase.RunCollisionPrewarmPhase(
                simBacklog, nextCollisionPrewarmAt, frameTimings.collisionPrewarmUs
            );
        const uint64_t simTicksThisLoop = m_simulationPhase.RunSimulationPhase(
            simAccumulator, serverTick, frameTimings.simUs, simBacklog, tickTimingsThisLoop
        );
        if (tickPerfLog) {
            ++tickPerfLoops;
            if (simBacklog) {
                ++tickPerfBacklogLoops;
            }
            const double tickSeconds = 1.0 / static_cast<double>(SimulationPhase::kTickRateHz);
            tickPerfMaxBacklogMs = std::max(
                tickPerfMaxBacklogMs, std::max(0.0, simAccumulator - tickSeconds) * 1000.0
            );
            for (const SimulationPhase::TickTimingSample &sample : tickTimingsThisLoop) {
                tickWorkSamplesMs.push_back(sample.workMs);
                if (sample.intervalMs > 0.0) {
                    tickIntervalSamplesMs.push_back(sample.intervalMs);
                }
            }
        }
        phaseStart = std::chrono::steady_clock::now();
        m_diagnosticsPhase.RunRespawnDiagnosticsPhase(simTicksThisLoop, serverTick);
        frameTimings.diagnosticsUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - phaseStart
            )
                .count()
        );
        frameTimings.snapshotUs =
            m_replicationPhase.RunSnapshotPhase(serverTick, lastSnapshotTime, snapshotInterval);
        phaseStart = std::chrono::steady_clock::now();
        const bool scoreboardBroadcastedThisLoop =
            m_gameplayPhase.RunScoreboardPhase(nextScoreboardBroadcastAt);
        frameTimings.scoreboardUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - phaseStart
            )
                .count()
        );
        const size_t chunkInterestTasksThisLoop =
            m_chunkInterestPhase.RunChunkInterestPhase(simBacklog, frameTimings.chunkInterestUs);
        const size_t chunksSentThisLoop = m_chunkSendPhase.RunChunkSendPhase(
            simBacklog, nextChunkSendFlushAt, frameTimings.chunkSendUs
        );

        const auto loopEnd = std::chrono::steady_clock::now();
        lastLoopWorkEnd = loopEnd;
        const double loopUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(loopEnd - loopStart).count()
        );
        if (tickPerfLog) {
            loopTimingSamples.push_back(
                LoopTimingSample{loopUs / 1000.0, loopWaitMs, loopIntervalMs, frameTimings}
            );
        }
        if (loopUs > perfLoopUsMax) {
            perfLoopUsMax = loopUs;
        }
        ++perfLoops;
        perfMessages += msgPacketsThisLoop;
        perfPlayerInputs += playerInputPacketsThisLoop;
        perfChunkRequests += chunkRequestPacketsThisLoop;
        perfSimTicks += simTicksThisLoop;
        perfCollisionPrewarmGenerated += collisionPrewarmGeneratedThisLoop;
        perfChunkInterestTasks += chunkInterestTasksThisLoop;
        perfChunksSent += chunksSentThisLoop;
        if (scoreboardBroadcastedThisLoop) {
            ++perfScoreboardBroadcasts;
        }
        perfLoopUsTotal += loopUs;
        perfMessageDrainUsTotal += frameTimings.messageDrainUs;
        perfSimUsTotal += frameTimings.simUs;
        perfCollisionPrewarmUsTotal += frameTimings.collisionPrewarmUs;
        perfSnapshotUsTotal += frameTimings.snapshotUs;
        perfChunkInterestUsTotal += frameTimings.chunkInterestUs;
        perfChunkSendUsTotal += frameTimings.chunkSendUs;

        if (DiagnosticsFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (loopUs >= kSlowServerLoopWarnUs || frameTimings.simUs >= kSlowServerSimWarnUs)) {
            std::cerr << "[perf/server] slow loopUs=" << loopUs << " simUs=" << frameTimings.simUs
                      << " msgDrainUs=" << frameTimings.messageDrainUs
                      << " prewarmUs=" << frameTimings.collisionPrewarmUs
                      << " snapshotUs=" << frameTimings.snapshotUs
                      << " chunkInterestUs=" << frameTimings.chunkInterestUs
                      << " chunkSendUs=" << frameTimings.chunkSendUs
                      << " simTicks=" << simTicksThisLoop << " msgs=" << msgPacketsThisLoop
                      << " inputs=" << playerInputPacketsThisLoop
                      << " chunkReq=" << chunkRequestPacketsThisLoop
                      << " prewarmGenerated=" << collisionPrewarmGeneratedThisLoop
                      << " chunkInterestTasks=" << chunkInterestTasksThisLoop
                      << " chunksSent=" << chunksSentThisLoop << "\n";
        }

        const auto perfNow = std::chrono::steady_clock::now();
        if (perfNow - tickPerfWindowStart >= kTickPerfLogInterval) {
            writeTickPerfWindow(perfNow);
        }
        if (DiagnosticsFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (perfNow - perfWindowStart) >= kServerPerfLogInterval) {
            const double loops = (perfLoops > 0) ? static_cast<double>(perfLoops) : 1.0;
            std::cerr << "[perf/server] 1s loops=" << perfLoops
                      << " avgLoopMs=" << (perfLoopUsTotal / loops) / 1000.0
                      << " maxLoopMs=" << perfLoopUsMax / 1000.0
                      << " avgMsgDrainMs=" << (perfMessageDrainUsTotal / loops) / 1000.0
                      << " avgSimMs=" << (perfSimUsTotal / loops) / 1000.0
                      << " avgPrewarmMs=" << (perfCollisionPrewarmUsTotal / loops) / 1000.0
                      << " avgSnapshotMs=" << (perfSnapshotUsTotal / loops) / 1000.0
                      << " avgChunkInterestMs=" << (perfChunkInterestUsTotal / loops) / 1000.0
                      << " avgChunkSendMs=" << (perfChunkSendUsTotal / loops) / 1000.0
                      << " simTicks=" << perfSimTicks << " msgs=" << perfMessages
                      << " inputs=" << perfPlayerInputs << " chunkReq=" << perfChunkRequests
                      << " prewarmGenerated=" << perfCollisionPrewarmGenerated
                      << " chunkInterestTasks=" << perfChunkInterestTasks
                      << " chunksSent=" << perfChunksSent
                      << " scoreboardBroadcasts=" << perfScoreboardBroadcasts << "\n";

            perfWindowStart = perfNow;
            perfLoops = 0;
            perfMessages = 0;
            perfPlayerInputs = 0;
            perfChunkRequests = 0;
            perfSimTicks = 0;
            perfCollisionPrewarmGenerated = 0;
            perfChunkInterestTasks = 0;
            perfChunksSent = 0;
            perfScoreboardBroadcasts = 0;
            perfLoopUsTotal = 0.0;
            perfLoopUsMax = 0.0;
            perfMessageDrainUsTotal = 0.0;
            perfSimUsTotal = 0.0;
            perfCollisionPrewarmUsTotal = 0.0;
            perfSnapshotUsTotal = 0.0;
            perfChunkInterestUsTotal = 0.0;
            perfChunkSendUsTotal = 0.0;
        }

        ApplyLoopPacing(simBacklog);
    }
    writeTickPerfWindow(std::chrono::steady_clock::now());
}
