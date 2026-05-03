#pragma once

struct ServerPlayer;
class ChunkManager;

namespace ServerMovementSimulation {
    void simulatePhysicsForPlayer(ServerPlayer &p, double dt, ChunkManager &chunkManager);
    void setMissingChunkCollisionDiagnosticsEnabled(bool enabled);
    bool isMissingChunkCollisionDiagnosticsEnabled();
} // namespace ServerMovementSimulation
