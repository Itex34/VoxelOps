#include "ClientReconciler.hpp"

#include "../application/AppHelpers.hpp"
#include "Runtime.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
inline bool IsNewerU32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline bool IsMoveInputActive(const NetworkInputState& input) {
    constexpr float kMoveDeadzone = 0.01f;
    const bool axisActive =
        (std::abs(input.moveX) > kMoveDeadzone) ||
        (std::abs(input.moveZ) > kMoveDeadzone);
    const uint8_t moveFlags =
        kPlayerInputFlagForward |
        kPlayerInputFlagBackward |
        kPlayerInputFlagLeft |
        kPlayerInputFlagRight |
        kPlayerInputFlagJump |
        kPlayerInputFlagFlyUp |
        kPlayerInputFlagFlyDown;
    const bool flagActive = (input.flags & moveFlags) != 0;
    return axisActive || flagActive;
}

inline bool HasActiveReplayInput(const Runtime& runtime) {
    for (const Runtime::PendingInputEntry& pending : runtime.pendingInputs) {
        NetworkInputState replayInput{};
        replayInput.moveX = pending.packet.moveX;
        replayInput.moveZ = pending.packet.moveZ;
        replayInput.flags = pending.packet.inputFlags;
        replayInput.flyMode = (pending.packet.flyMode != 0);
        if (IsMoveInputActive(replayInput)) {
            return true;
        }
    }
    return false;
}
}

bool ClientReconciler::Apply(Runtime& runtime, const ServerSnapshot& snapshot)
{
    if (runtime.hasAppliedServerTick && !IsNewerU32(snapshot.serverTick, runtime.lastAppliedServerTick)) {
        return false;
    }

    const bool wasAlive = runtime.localPlayerAlive;
    runtime.localPlayerAlive = snapshot.alive;
    runtime.localRespawnSeconds = snapshot.respawnSeconds;
    if (wasAlive && !runtime.localPlayerAlive) {
        runtime.pendingInputs.clear();
        runtime.localSimAccumulator = 0.0;
    }
    if (!wasAlive && runtime.localPlayerAlive) {
        // Drop any stale pre-respawn inputs so replay starts from fresh movement.
        runtime.pendingInputs.clear();
        runtime.localSimAccumulator = 0.0;
        runtime.renderStateNeedsResync = true;
        runtime.hasSmoothedPlayerCameraPos = false;
        runtime.localDeathKiller.clear();
    }

    runtime.hasAppliedServerTick = true;
    runtime.lastAppliedServerTick = snapshot.serverTick;
    runtime.lastAckedInputTick = snapshot.ackedInputTick;
    while (!runtime.pendingInputs.empty() &&
        AppHelpers::IsAckedU32(runtime.pendingInputs.front().packet.inputTick, runtime.lastAckedInputTick)) {
        runtime.pendingInputs.pop_front();
    }

    runtime.player->setFlyModeAllowed(snapshot.allowFlyMode);
    const Player::SimulationState predictedState = runtime.player->captureSimulationState();
    const glm::vec3 predictedPos = predictedState.position;

    Player::SimulationState serverBaseState = predictedState;
    serverBaseState.position = snapshot.position;
    serverBaseState.velocity = snapshot.velocity;
    serverBaseState.onGround = snapshot.onGround;
    serverBaseState.flyMode = snapshot.flyMode;
    serverBaseState.jumpPressedLastTick = snapshot.jumpPressedLastTick;
    serverBaseState.timeSinceGrounded = std::max(0.0f, snapshot.timeSinceGrounded);
    serverBaseState.jumpBufferTimer = std::max(0.0f, snapshot.jumpBufferTimer);
    runtime.player->restoreSimulationState(serverBaseState);

    constexpr size_t kMaxReplaySteps = 64;
    size_t replayCount = 0;
    for (const Runtime::PendingInputEntry& pending : runtime.pendingInputs) {
        if (replayCount >= kMaxReplaySteps) {
            static uint32_t s_replayCapHitCount = 0;
            ++s_replayCapHitCount;
            if (s_replayCapHitCount <= 20 || (s_replayCapHitCount % 100) == 0) {
                std::cerr
                    << "[reconcile] replay cap hit pending=" << runtime.pendingInputs.size()
                    << " serverTick=" << snapshot.serverTick
                    << " ackedInputTick=" << snapshot.ackedInputTick
                    << "\n";
            }
            break;
        }
        NetworkInputState replayInput{};
        replayInput.moveX = pending.packet.moveX;
        replayInput.moveZ = pending.packet.moveZ;
        replayInput.yaw = pending.packet.yaw;
        replayInput.pitch = pending.packet.pitch;
        replayInput.flags = pending.packet.inputFlags;
        replayInput.flyMode =
            snapshot.allowFlyMode &&
            (pending.packet.flyMode != 0);
        runtime.player->simulateFromNetworkInput(replayInput, Runtime::LocalPredictionStep, false);
        ++replayCount;
    }

    Player::SimulationState reconciledState = runtime.player->captureSimulationState();
    // Keep local look/camera orientation on immediate mouse timeline.
    // Movement replay uses world-space move axes, so server reconciliation should
    // not pull view yaw/pitch backward to older packet samples.
    reconciledState.yaw = predictedState.yaw;
    reconciledState.pitch = predictedState.pitch;
    reconciledState.front = predictedState.front;
    reconciledState.currentFov = predictedState.currentFov;
    const glm::vec3 simCorrection = reconciledState.position - predictedPos;
    const float latencyBlend = AppHelpers::LatencyCorrectionBlend(runtime.clientNet);
    const float latencyCurve = latencyBlend * latencyBlend;
    const float softTeleportDist = Runtime::BasicAuthReconcileTeleportDistance + 1.5f + (3.0f * latencyCurve);
    const float hardSnapDist = softTeleportDist + 5.0f + (4.0f * latencyCurve);
    const float hardSnapDistSq = hardSnapDist * hardSnapDist;

    Player::SimulationState finalState = reconciledState;
    const bool airborne = !snapshot.onGround || !predictedState.onGround || !reconciledState.onGround;
    if (airborne) {
        // Jump arcs are very sensitive to tiny Y corrections. Keep local vertical
        // continuity unless the correction is large enough to matter.
        const float verticalAirDeadzone = 0.12f + (0.10f * latencyBlend);
        if (std::abs(simCorrection.y) <= verticalAirDeadzone) {
            finalState.position.y = predictedState.position.y;
            finalState.velocity.y = predictedState.velocity.y;
            finalState.onGround = predictedState.onGround;
        }
    }

    const glm::vec3 effectiveCorrection = finalState.position - predictedPos;
    const float effectiveCorrectionLenSq = glm::dot(effectiveCorrection, effectiveCorrection);
    const float microDeadzone =
        Runtime::BasicAuthReconcileDeadzone * (0.60f + (0.40f * latencyBlend));
    const float microDeadzoneSq = microDeadzone * microDeadzone;

    const bool allowIdleSnap = latencyBlend >= 0.25f;
    constexpr float kIdleVelocityEps = 0.15f;
    const bool inputIdle =
        !HasActiveReplayInput(runtime) &&
        !IsMoveInputActive(runtime.player->getNetworkInputState());
    const bool serverNearlyStopped = glm::length(snapshot.velocity) <= kIdleVelocityEps;
    const float idleSnapDist = Runtime::BasicAuthReconcileDeadzone * 2.0f + (0.7f * latencyCurve);
    const float idleSnapDistSq = idleSnapDist * idleSnapDist;

    if (allowIdleSnap && inputIdle && snapshot.onGround && serverNearlyStopped && effectiveCorrectionLenSq <= idleSnapDistSq) {
        runtime.player->restoreSimulationState(finalState);
        runtime.renderStateNeedsResync = true;
        return true;
    }

    if (effectiveCorrectionLenSq <= microDeadzoneSq) {
        // Tiny corrections create visible "buzz" if applied every snapshot.
        // Let them accumulate until they become meaningful.
        runtime.player->restoreSimulationState(predictedState);
        return true;
    }

    if (effectiveCorrectionLenSq > hardSnapDistSq) {
        runtime.player->restoreSimulationState(finalState);
        runtime.renderStateNeedsResync = true;
    }
    else {
        // For non-teleport corrections, blend over time to avoid jagged motion.
        const float correctionLen = std::sqrt(effectiveCorrectionLenSq);
        const float correctionT = std::clamp(
            correctionLen / std::max(softTeleportDist, 1e-4f),
            0.0f,
            1.0f
        );
        const float basePosBlend = finalState.onGround ? 0.24f : 0.16f;
        const float posBlend = std::clamp(basePosBlend + (0.36f * correctionT), basePosBlend, 0.78f);
        const float velBlend = std::clamp(0.30f + (0.45f * correctionT), 0.30f, 0.85f);

        Player::SimulationState blendedState = finalState;
        blendedState.position = predictedState.position + (effectiveCorrection * posBlend);
        blendedState.velocity = glm::mix(predictedState.velocity, finalState.velocity, velBlend);
        runtime.player->restoreSimulationState(blendedState);

        // Only trigger resync for large corrections to avoid visual stutter
        if (effectiveCorrectionLenSq > (softTeleportDist * softTeleportDist)) {
            runtime.renderStateNeedsResync = true;
        }
    }

    return true;
}
