#include "ServerDiagnosticsFlags.hpp"

namespace ServerDiagFlags {

std::atomic<bool> g_enableChunkDiagnostics{false};
std::atomic<bool> g_enableServerPerfDiagnostics{true};
std::atomic<bool> g_enableRespawnRubberbandDiagnostics{true};

} // namespace ServerDiagFlags
