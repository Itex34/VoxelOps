#pragma once

struct ServerPlayer;
class ChunkManager;
struct PlayerInput;

namespace ServerMovementSimulation {
    void simulatePhysicsForPlayer(ServerPlayer &p, double dt, ChunkManager &chunkManager);
    void simulatePhysicsForPlayerPrepared(
        ServerPlayer &p,
        double dt,
        ChunkManager &chunkManager,
        const PlayerInput *preparedInput
    );
    void setMissingChunkCollisionDiagnosticsEnabled(bool enabled);
    bool isMissingChunkCollisionDiagnosticsEnabled();
} // namespace ServerMovementSimulation
