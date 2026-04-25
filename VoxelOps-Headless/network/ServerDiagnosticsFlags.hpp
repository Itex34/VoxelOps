#pragma once

#include <atomic>

namespace ServerDiagFlags {

extern std::atomic<bool> g_enableChunkDiagnostics;
extern std::atomic<bool> g_enableServerPerfDiagnostics;
extern std::atomic<bool> g_enableRespawnRubberbandDiagnostics;

} // namespace ServerDiagFlags
