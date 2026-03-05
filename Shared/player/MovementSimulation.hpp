#pragma once

#include "../network/Packets.hpp"
#include "PlayerData.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Shared::Movement {

struct InputState {
    float moveX = 0.0f;
    float moveZ = 0.0f;
    uint8_t flags = 0;
    bool flyMode = false;
};

struct State {
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    bool onGround = false;
    bool flyMode = false;
    bool jumpPressedLastTick = false;
    float timeSinceGrounded = 0.0f;
    float jumpBufferTimer = 0.0f;
};

struct Options {
    bool allowFlyMode = false;
    bool allowStepUp = true;
    bool requireSprintForStepUp = true;
};

inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

template <typename CollisionFn>
inline float MoveAndCollide(
    State& state,
    const glm::vec3& delta,
    const PlayerData::MovementSettings& movement,
    bool allowStepUp,
    CollisionFn&& collides
) {
    float stepUpHeight = 0.0f;
    const bool wasGrounded = state.onGround;

    if (state.flyMode) {
        state.position += delta;
        return 0.0f;
    }

    glm::vec3 tryPos = state.position;
    bool steppedDuringMove = false;

    const glm::vec3 xBasePos = tryPos;
    tryPos.x += delta.x;
    if (collides(tryPos)) {
        bool stepped = false;
        if (allowStepUp && state.onGround && std::abs(delta.x) > 1e-6f) {
            for (float step = movement.stepIncrement; step <= movement.maxStepHeight + 1e-6f; step += movement.stepIncrement) {
                glm::vec3 testPos = xBasePos;
                testPos.y += step;
                testPos.x += delta.x;
                if (!collides(testPos)) {
                    tryPos.x = testPos.x;
                    tryPos.y = std::max(tryPos.y, testPos.y);
                    stepUpHeight = std::max(stepUpHeight, testPos.y - state.position.y);
                    state.velocity.y = 0.0f;
                    stepped = true;
                    steppedDuringMove = true;
                    break;
                }
            }
        }

        if (!stepped) {
            tryPos.x = state.position.x;
            state.velocity.x = 0.0f;
        }
    }

    const glm::vec3 zBasePos = tryPos;
    tryPos.z += delta.z;
    if (collides(tryPos)) {
        bool stepped = false;
        if (allowStepUp && state.onGround && std::abs(delta.z) > 1e-6f) {
            for (float step = movement.stepIncrement; step <= movement.maxStepHeight + 1e-6f; step += movement.stepIncrement) {
                glm::vec3 testPos = zBasePos;
                testPos.y += step;
                testPos.z += delta.z;
                if (!collides(testPos)) {
                    tryPos.z = testPos.z;
                    tryPos.y = std::max(tryPos.y, testPos.y);
                    stepUpHeight = std::max(stepUpHeight, testPos.y - state.position.y);
                    state.velocity.y = 0.0f;
                    stepped = true;
                    steppedDuringMove = true;
                    break;
                }
            }
        }

        if (!stepped) {
            tryPos.z = state.position.z;
            state.velocity.z = 0.0f;
        }
    }

    state.onGround = false;
    if (!steppedDuringMove) {
        glm::vec3 tryPosY = tryPos;
        tryPosY.y += delta.y;
        if (!collides(tryPosY)) {
            tryPos = tryPosY;
        }
        else {
            if (delta.y < 0.0f) {
                state.onGround = true;
                state.velocity.y = 0.0f;
                if (wasGrounded) {
                    tryPos.y = state.position.y;
                }
                else {
                    float lowY = tryPosY.y;
                    float highY = tryPos.y;
                    for (int i = 0; i < 10; ++i) {
                        const float midY = 0.5f * (lowY + highY);
                        glm::vec3 testPos = tryPos;
                        testPos.y = midY;
                        if (collides(testPos)) {
                            lowY = midY;
                        }
                        else {
                            highY = midY;
                        }
                    }
                    tryPos.y = highY;
                }
            }
            else {
                state.velocity.y = 0.0f;
                tryPos.y = state.position.y;
            }
        }
    }
    else {
        state.onGround = true;
        state.velocity.y = 0.0f;
    }

    if (steppedDuringMove && stepUpHeight > 0.0f) {
        // Stepping up should cost horizontal momentum, especially for tall steps.
        const float heightRatio = std::clamp(
            stepUpHeight / std::max(movement.maxStepHeight, 1e-4f),
            0.0f,
            1.0f
        );
        const float slowdown = std::clamp(
            1.0f - movement.stepUpHorizontalSlowdown * heightRatio,
            0.2f,
            1.0f
        );
        // Apply slowdown to this frame's horizontal travel, not only velocity,
        // because ground acceleration can restore velocity immediately on next tick.
        tryPos.x = state.position.x + (tryPos.x - state.position.x) * slowdown;
        tryPos.z = state.position.z + (tryPos.z - state.position.z) * slowdown;
        state.velocity.x *= slowdown;
        state.velocity.z *= slowdown;
    }

    state.position = tryPos;
    return stepUpHeight;
}

template <typename CollisionFn>
inline void Simulate(
    State& state,
    const InputState& input,
    float dt,
    const PlayerData::MovementSettings& movement,
    const Options& options,
    CollisionFn&& collides,
    float* outStepUpHeight = nullptr
) {
    dt = std::max(0.0f, dt);

    state.flyMode = options.allowFlyMode && input.flyMode;

    glm::vec2 moveInput(std::clamp(input.moveX, -1.0f, 1.0f), std::clamp(input.moveZ, -1.0f, 1.0f));
    if (glm::length(moveInput) > 1.0f) {
        moveInput = glm::normalize(moveInput);
    }

    const bool sprint = (input.flags & kPlayerInputFlagSprint) != 0;
    const float targetSpeed = sprint ? movement.sprintSpeed : movement.walkSpeed;
    const glm::vec3 desiredHorizontal(moveInput.x * targetSpeed, 0.0f, moveInput.y * targetSpeed);

    if (state.flyMode) {
        float verticalInput = 0.0f;
        if ((input.flags & kPlayerInputFlagFlyUp) != 0) verticalInput += 1.0f;
        if ((input.flags & kPlayerInputFlagFlyDown) != 0) verticalInput -= 1.0f;

        state.velocity = desiredHorizontal;
        state.velocity.y = verticalInput * targetSpeed;
        const float steppedHeight = MoveAndCollide(
            state,
            state.velocity * dt,
            movement,
            false,
            std::forward<CollisionFn>(collides)
        );
        if (outStepUpHeight != nullptr) {
            *outStepUpHeight = steppedHeight;
        }
        state.onGround = false;
        state.timeSinceGrounded = 0.0f;
        state.jumpBufferTimer = 0.0f;
        state.jumpPressedLastTick = (input.flags & kPlayerInputFlagJump) != 0;
        return;
    }

    const bool jumpPressed = (input.flags & kPlayerInputFlagJump) != 0;
    if (state.onGround) {
        state.timeSinceGrounded = 0.0f;
    }
    else {
        state.timeSinceGrounded += dt;
    }
    if (jumpPressed && !state.jumpPressedLastTick) {
        state.jumpBufferTimer = movement.jumpBufferSec;
    }
    else {
        state.jumpBufferTimer = std::max(0.0f, state.jumpBufferTimer - dt);
    }

    const float accel = state.onGround ? movement.groundAcceleration : movement.airAcceleration;
    const float alpha = std::clamp(accel * dt, 0.0f, 1.0f);
    state.velocity.x = Lerp(state.velocity.x, desiredHorizontal.x, alpha);
    state.velocity.z = Lerp(state.velocity.z, desiredHorizontal.z, alpha);

    state.velocity.y += movement.gravity * dt;
    if (state.velocity.y < -movement.terminalVelocity) {
        state.velocity.y = -movement.terminalVelocity;
    }

    const bool canJump = state.onGround || (state.timeSinceGrounded <= movement.coyoteTimeSec);
    if (state.jumpBufferTimer > 0.0f && canJump) {
        const bool hasMoveIntent = glm::length(moveInput) > movement.sprintJumpMinMoveInput;
        const bool useSprintJump = sprint && hasMoveIntent;
        state.velocity.y = useSprintJump
            ? movement.jumpVelocity * movement.sprintJumpVelocityMultiplier
            : movement.jumpVelocity;
        state.onGround = false;
        state.timeSinceGrounded = movement.coyoteTimeSec + dt;
        state.jumpBufferTimer = 0.0f;
    }
    state.jumpPressedLastTick = jumpPressed;

    const bool allowStepUpForTick =
        options.allowStepUp && (!options.requireSprintForStepUp || sprint);
    const float steppedHeight = MoveAndCollide(
        state,
        state.velocity * dt,
        movement,
        allowStepUpForTick,
        std::forward<CollisionFn>(collides)
    );
    if (outStepUpHeight != nullptr) {
        *outStepUpHeight = steppedHeight;
    }

    if (state.onGround) {
        state.timeSinceGrounded = 0.0f;
    }

    if (state.onGround && glm::length(moveInput) < 0.001f) {
        const float frictionAlpha = std::clamp(movement.groundFriction * dt, 0.0f, 1.0f);
        state.velocity.x = Lerp(state.velocity.x, 0.0f, frictionAlpha);
        state.velocity.z = Lerp(state.velocity.z, 0.0f, frictionAlpha);
    }
}

} // namespace Shared::Movement
