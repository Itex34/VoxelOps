#include "ServerDiagnosticsFlags.hpp"

namespace ServerDiagFlags {

std::atomic<bool> g_enableChunkDiagnostics{false};
std::atomic<bool> g_enableServerPerfDiagnostics{false};
std::atomic<bool> g_enableRespawnRubberbandDiagnostics{false};

void SetAllEnabled(bool enabled) {
    g_enableChunkDiagnostics.store(enabled, std::memory_order_release);
    g_enableServerPerfDiagnostics.store(enabled, std::memory_order_release);
    g_enableRespawnRubberbandDiagnostics.store(enabled, std::memory_order_release);
}

bool IsAnyEnabled() {
    return g_enableChunkDiagnostics.load(std::memory_order_acquire) ||
           g_enableServerPerfDiagnostics.load(std::memory_order_acquire) ||
           g_enableRespawnRubberbandDiagnostics.load(std::memory_order_acquire);
}

} // namespace ServerDiagFlags
