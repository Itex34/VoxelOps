#pragma once

#include <atomic>

namespace DiagnosticsFlags {

    extern std::atomic<bool> g_enableChunkDiagnostics;
    extern std::atomic<bool> g_enableServerPerfDiagnostics;
    extern std::atomic<bool> g_enableRespawnRubberbandDiagnostics;

    void SetAllEnabled(bool enabled);
    bool IsAnyEnabled();

} // namespace DiagnosticsFlags
