#pragma once

namespace Shared {
    namespace PlayerData {

        struct MovementSettings {
            float gravity = -53.0f;
            float terminalVelocity = 90.0f;
            float groundAcceleration = 120.0f;
            float airAcceleration = 20.0f;
            float groundFriction = 20.0f;
            float maxStepHeight = 1.0f;
            float stepIncrement = 0.05f;

            float stepCooldownSec = 0.18f;
            float stepMinHorizontalSpeed = 0.8f;
            float stepUpHorizontalSlowdown = 0.35f;
            float stepRecoverySpeedMultiplier = 0.45f;

            float walkSpeed = 8.0f;
            float sprintSpeed = 13.0f;
            float jumpVelocity = 10.6f;
            float sprintJumpVelocityMultiplier = 1.4f;
            float sprintJumpMinMoveInput = 0.2f;
            float coyoteTimeSec = 0.1f; //how long you can jump after walking off a block
            float jumpBufferSec = 0.1f;

            float collisionHeight = 2.56f;
            float collisionRadius = 0.3f;
            float eyeHeight = 2.2f;

            // Input silence guard for server authoritative movement.
            // If inputs stop arriving, decay movement intent and then force-stop.
            float inputSilenceDecayStartSec = 0.15f;
            float inputSilenceStopSec = 0.35f;
        };

        const MovementSettings &GetMovementSettings();

    } // namespace PlayerData
} // namespace Shared
