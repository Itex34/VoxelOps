#pragma once

#include "ServerPlayer.hpp"

class ChunkManager;

namespace ServerMovementSimulation {
    struct PlayerPhysicsState {
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool onGround = false;
        float height = 2.56f;
        float radius = 0.3f;
        Clock::time_point lastInputReceived = Clock::now();
        uint8_t activeInputFlags = 0;
        bool flyMode = false;
        bool allowFlyMode = false;
        float moveX = 0.0f;
        float moveZ = 0.0f;
        bool jumpPressedLastTick = false;
        float timeSinceGrounded = 0.0f;
        float jumpBufferTimer = 0.0f;
        float stepCooldownTimer = 0.0f;
        GrappleState grappleState{};
    };

    PlayerPhysicsState ExtractPhysicsState(const ServerPlayer &player);
    void ApplyPhysicsState(ServerPlayer &player, const PlayerPhysicsState &state);
    void simulatePhysicsForPlayerPrepared(
        PlayerPhysicsState &p,
        double dt,
        ChunkManager &chunkManager,
        const PlayerInput *preparedInput
    );
    void simulatePhysicsForPlayerPrepared(
        ServerPlayer &p,
        double dt,
        ChunkManager &chunkManager,
        const PlayerInput *preparedInput
    );
    void simulatePhysicsForPlayer(ServerPlayer &p, double dt, ChunkManager &chunkManager);
    void setMissingChunkCollisionDiagnosticsEnabled(bool enabled);
    bool isMissingChunkCollisionDiagnosticsEnabled();
} // namespace ServerMovementSimulation
