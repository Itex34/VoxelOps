#include "ServerMovementSimulation.hpp"
#include "ServerPlayer.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../../Shared/network/Packets.hpp"
#include "../../../Shared/player/MovementSimulation.hpp"
#include "../../../Shared/player/GrappleSwing.hpp"
#include "../../../Shared/player/PlayerData.hpp"
#include "../../../Shared/utils/Math.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>

namespace ServerMovementSimulation {
    namespace {
        constexpr bool kServerBlockOnMissingCollisionChunk = true;
        std::atomic<uint64_t> g_missingChunkCollisionCount{0};
        std::atomic<bool> g_enableMissingChunkCollisionDiagnostics{false};

        bool
        checkCollision(const ServerPlayer &p, const glm::vec3 &pos, ChunkManager &chunkManager) {
            if (p.flyMode)
                return false;
            const ChunkManager::AabbCollisionQueryResult query = chunkManager.queryAabbCollision(
                pos, p.radius, p.height, kServerBlockOnMissingCollisionChunk
            );
            if (query.missingChunk &&
                g_enableMissingChunkCollisionDiagnostics.load(std::memory_order_acquire)) {
                const uint64_t count =
                    g_missingChunkCollisionCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 40 || (count % 200) == 0) {
                    std::cerr << "[perf/player-manager] missing collision chunk=("
                              << query.firstMissingChunk.x << "," << query.firstMissingChunk.y
                              << "," << query.firstMissingChunk.z << ")"
                              << " playerPos=(" << p.position.x << "," << p.position.y << ","
                              << p.position.z << ")"
                              << " count=" << count << "\n";
                }
            }
            return query.collided;
        }
    } // namespace

    void simulatePhysicsForPlayer(ServerPlayer &p, double dt, ChunkManager &chunkManager) {
        const auto &movement = Shared::PlayerData::GetMovementSettings();
        constexpr uint8_t kContinuousMoveFlags = kPlayerInputFlagForward |
                                                 kPlayerInputFlagBackward | kPlayerInputFlagLeft |
                                                 kPlayerInputFlagRight | kPlayerInputFlagSprint |
                                                 kPlayerInputFlagFlyUp | kPlayerInputFlagFlyDown;

        PlayerInput cmd{};
        if (p.inputBuffer.consumeNext(cmd)) {
            const float safeYaw = std::isfinite(cmd.yaw) ? cmd.yaw : 0.0f;
            const float safePitch = std::isfinite(cmd.pitch) ? cmd.pitch : 0.0f;
            const float safeMoveX = std::isfinite(cmd.moveX) ? cmd.moveX : 0.0f;
            const float safeMoveZ = std::isfinite(cmd.moveZ) ? cmd.moveZ : 0.0f;

            p.activeInputFlags = cmd.inputFlags;
            p.flyMode = p.allowFlyMode && (cmd.flyMode != 0);
            // Keep look values only for replication/debug; they are not used for authoritative
            // movement.
            p.yaw = Shared::Utils::NormalizeYawDegrees(safeYaw);
            p.pitch = safePitch;
            p.moveX = std::clamp(safeMoveX, -1.0f, 1.0f);
            p.moveZ = std::clamp(safeMoveZ, -1.0f, 1.0f);
        }

        uint8_t effectiveFlags = p.activeInputFlags;
        float effectiveMoveX = p.moveX;
        float effectiveMoveZ = p.moveZ;
        if (movement.inputSilenceStopSec > 0.0f &&
            movement.inputSilenceStopSec > movement.inputSilenceDecayStartSec) {
            const auto now = Clock::now();
            const float silenceSec =
                std::chrono::duration<float>(now - p.lastInputReceived).count();
            if (silenceSec > movement.inputSilenceDecayStartSec) {
                const float t = std::clamp(
                    (silenceSec - movement.inputSilenceDecayStartSec) /
                        (movement.inputSilenceStopSec - movement.inputSilenceDecayStartSec),
                    0.0f,
                    1.0f
                );
                const float keep = 1.0f - t;
                effectiveMoveX *= keep;
                effectiveMoveZ *= keep;
                if (t >= 1.0f) {
                    effectiveFlags &= static_cast<uint8_t>(~kContinuousMoveFlags);
                }
            }
        }

        Shared::Movement::State simState;
        simState.position = p.position;
        simState.velocity = p.velocity;
        simState.onGround = p.onGround;
        simState.flyMode = p.flyMode;
        simState.jumpPressedLastTick = p.jumpPressedLastTick;
        simState.timeSinceGrounded = p.timeSinceGrounded;
        simState.jumpBufferTimer = p.jumpBufferTimer;
        if (!simState.flyMode && p.grappleState.active) {
            const double nowSeconds = std::chrono::duration<double>(
                                          std::chrono::steady_clock::now().time_since_epoch()
            )
                                          .count();
            constexpr double kReelCommandTimeoutSeconds = 0.20;
            if (p.grappleState.reelingIn &&
                (nowSeconds - p.grappleState.lastReelCommandTime) <= kReelCommandTimeoutSeconds) {
                Shared::Grapple::ApplyReelIn(
                    p.grappleState.ropeLength,
                    static_cast<float>(dt)
                );
            } else {
                p.grappleState.reelingIn = false;
            }

            Shared::Grapple::ApplyRopeConstraint(
                p.grappleState.anchor,
                p.grappleState.ropeLength,
                simState.position,
                simState.velocity,
                [&p, &chunkManager](const glm::vec3 &testPos) {
                    return checkCollision(p, testPos, chunkManager);
                }
            );
        }

        Shared::Movement::InputState simInput;
        simInput.moveX = effectiveMoveX;
        simInput.moveZ = effectiveMoveZ;
        simInput.flags = effectiveFlags;
        simInput.flyMode = p.flyMode;

        Shared::Movement::Options simOptions;
        simOptions.allowFlyMode = p.allowFlyMode;
        simOptions.allowStepUp = true;
        simOptions.requireSprintForStepUp = true;
        simOptions.disableAirControlWhenAirborne = p.grappleState.active;

        Shared::Movement::Simulate(
            simState,
            simInput,
            static_cast<float>(dt),
            movement,
            simOptions,
            [&p, &chunkManager](const glm::vec3 &testPos) {
                return checkCollision(p, testPos, chunkManager);
            },
            nullptr
        );

        p.position = simState.position;
        p.velocity = simState.velocity;
        p.onGround = simState.onGround;
        p.flyMode = simState.flyMode;
        p.jumpPressedLastTick = simState.jumpPressedLastTick;
        p.timeSinceGrounded = simState.timeSinceGrounded;
        p.jumpBufferTimer = simState.jumpBufferTimer;
    }

    void setMissingChunkCollisionDiagnosticsEnabled(bool enabled) {
        g_enableMissingChunkCollisionDiagnostics.store(enabled, std::memory_order_release);
    }

    bool isMissingChunkCollisionDiagnosticsEnabled() {
        return g_enableMissingChunkCollisionDiagnostics.load(std::memory_order_acquire);
    }

} // namespace ServerMovementSimulation
