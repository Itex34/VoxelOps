#include "ServerRuntime.hpp"
#include "ServerDiagnosticsFlags.hpp"

namespace {
constexpr auto kServerPerfLogInterval = std::chrono::seconds(1);
constexpr double kSlowServerLoopWarnUs = 12000.0;
constexpr double kSlowServerSimWarnUs = 5000.0;

struct FrameTimings {
    double messageDrainUs = 0.0;
    double simUs = 0.0;
    double snapshotUs = 0.0;
    double chunkInterestUs = 0.0;
    double chunkSendUs = 0.0;
    double collisionPrewarmUs = 0.0;
};

} // namespace

void ServerRuntime::ApplyLoopPacingPhase(bool simBacklog) {
    if (simBacklog) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ServerRuntime::MainLoop() {
    auto lastFrameTime = std::chrono::steady_clock::now();
    auto lastSnapshotTime = lastFrameTime;
    constexpr uint32_t kSnapshotSendRateHz =
        60u; // Match server tick rate for smooth reconciliation (CS:GO/Valorant style)
    constexpr double kSnapshotSendSeconds = 1.0 / static_cast<double>(kSnapshotSendRateHz);
    const auto snapshotInterval = std::chrono::duration<double>(kSnapshotSendSeconds);

    double simAccumulator = 0.0;
    uint32_t serverTick = 0;
    auto nextChunkSendFlushAt = std::chrono::steady_clock::now();
    auto nextCollisionPrewarmAt = std::chrono::steady_clock::now();
    auto perfWindowStart = std::chrono::steady_clock::now();
    auto nextScoreboardBroadcastAt = std::chrono::steady_clock::now();

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

    while (!m_quit) {
        const auto loopStart = std::chrono::steady_clock::now();

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

        RunInboundMessagePhase(msgPacketsThisLoop, playerInputPacketsThisLoop,
                               chunkRequestPacketsThisLoop, frameTimings.messageDrainUs);
        RunConnectionCleanupPhase();
        const uint64_t simTicksThisLoop =
            RunSimulationPhase(simAccumulator, serverTick, frameTimings.simUs, simBacklog);
        RunRespawnDiagnosticsPhase(simTicksThisLoop, serverTick);
        frameTimings.snapshotUs = RunSnapshotPhase(serverTick, lastSnapshotTime, snapshotInterval);
        const bool scoreboardBroadcastedThisLoop = RunScoreboardPhase(nextScoreboardBroadcastAt);
        const size_t chunkInterestTasksThisLoop =
            RunChunkInterestPhase(simBacklog, frameTimings.chunkInterestUs);
        const size_t chunksSentThisLoop =
            RunChunkSendPhase(simBacklog, nextChunkSendFlushAt, frameTimings.chunkSendUs);
        const size_t collisionPrewarmGeneratedThisLoop =
            RunCollisionPrewarmPhase(simBacklog, nextCollisionPrewarmAt,
                                     frameTimings.collisionPrewarmUs);

        const double loopUs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - loopStart)
                                    .count());
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

        if (ServerDiagFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (loopUs >= kSlowServerLoopWarnUs || frameTimings.simUs >= kSlowServerSimWarnUs)) {
            std::cerr << "[perf/server] slow loopUs=" << loopUs
                      << " simUs=" << frameTimings.simUs
                      << " msgDrainUs=" << frameTimings.messageDrainUs
                      << " prewarmUs=" << frameTimings.collisionPrewarmUs
                      << " snapshotUs=" << frameTimings.snapshotUs
                      << " chunkInterestUs=" << frameTimings.chunkInterestUs
                      << " chunkSendUs=" << frameTimings.chunkSendUs
                      << " simTicks=" << simTicksThisLoop
                      << " msgs=" << msgPacketsThisLoop << " inputs=" << playerInputPacketsThisLoop
                      << " chunkReq=" << chunkRequestPacketsThisLoop
                      << " prewarmGenerated=" << collisionPrewarmGeneratedThisLoop
                      << " chunkInterestTasks=" << chunkInterestTasksThisLoop
                      << " chunksSent=" << chunksSentThisLoop << "\n";
        }

        const auto perfNow = std::chrono::steady_clock::now();
        if (ServerDiagFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
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

        ApplyLoopPacingPhase(simBacklog);
    }
}

// Broadcast raw payload to everyone except `except`
void ServerRuntime::SetDebugLoggingEnabled(bool enabled) {
    ServerDiagFlags::SetAllEnabled(enabled);
    m_playerManager.SetDebugLoggingEnabled(enabled);
    std::cout << "[debug] diagnostics " << (enabled ? "enabled" : "disabled") << "\n";
}

bool ServerRuntime::IsDebugLoggingEnabled() {
    if (ServerDiagFlags::IsAnyEnabled()) {
        return true;
    }
    return m_playerManager.IsDebugLoggingEnabled();
}
