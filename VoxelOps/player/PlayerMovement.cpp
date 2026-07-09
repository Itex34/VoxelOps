#include "Player.hpp"

#include "../world/ChunkManager.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/player/GrappleSwing.hpp"
#include "../../Shared/player/MovementSimulation.hpp"
#include "../../Shared/network/Packets.hpp"

#include <algorithm>
#include <cmath>

namespace {

inline const Shared::PlayerData::MovementSettings &movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}

constexpr bool kClientBlockOnMissingCollisionChunk = true;
constexpr float kCollisionSkin = 0.001f;

float NormalizeYawDegrees(float yawDegrees) {
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f) {
        y -= 360.0f;
    }
    if (y < -180.0f) {
        y += 360.0f;
    }
    return y;
}

int ifloor(float v) {
    return static_cast<int>(std::floor(v));
}

} // namespace

bool Player::checkCollision(const glm::vec3 &pos) const {
    if (flyMode) {
        return false;
    }

    const float minX = pos.x - playerRadius + kCollisionSkin;
    const float maxX = pos.x + playerRadius - kCollisionSkin;
    const float minY = pos.y + kCollisionSkin;
    const float maxY = pos.y + playerHeight - kCollisionSkin;
    const float minZ = pos.z - playerRadius + kCollisionSkin;
    const float maxZ = pos.z + playerRadius - kCollisionSkin;

    const int ix0 = ifloor(minX);
    const int iy0 = ifloor(minY);
    const int iz0 = ifloor(minZ);
    const int ix1 = ifloor(maxX);
    const int iy1 = ifloor(maxY);
    const int iz1 = ifloor(maxZ);

    for (int x = ix0; x <= ix1; ++x) {
        for (int y = iy0; y <= iy1; ++y) {
            for (int z = iz0; z <= iz1; ++z) {
                const glm::ivec3 worldPos(x, y, z);
                const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
                if (chunkManager.inBounds(chunkPos) && !chunkManager.hasChunkLoaded(chunkPos)) {
                    if (kClientBlockOnMissingCollisionChunk && m_treatMissingCollisionAsSolid) {
                        return true;
                    }
                    continue;
                }
                if (chunkManager.getBlockGlobal(x, y, z) != BlockID::Air) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Player::moveAndCollide(const glm::vec3 &delta, bool allowStepUp, float *outStepUpHeight) {
    Shared::Movement::State state;
    state.position = position;
    state.velocity = velocity;
    state.onGround = onGround;
    state.flyMode = flyMode;
    state.jumpPressedLastTick = m_jumpPressedLastTick;
    state.timeSinceGrounded = m_timeSinceGrounded;
    state.jumpBufferTimer = m_jumpBufferTimer;
    state.stepCooldownTimer = m_stepCooldownTimer;

    const float stepUpHeight = Shared::Movement::MoveAndCollide(
        state,
        delta,
        movementSettings(),
        allowStepUp,
        [this](const glm::vec3 &testPos) { return checkCollision(testPos); }
    );

    position = state.position;
    velocity = state.velocity;
    onGround = state.onGround;
    m_stepCooldownTimer = state.stepCooldownTimer;

    if (outStepUpHeight != nullptr) {
        *outStepUpHeight = stepUpHeight;
    }
}

Player::SimulationState Player::captureSimulationState() const noexcept {
    SimulationState state;
    state.position = position;
    state.velocity = velocity;
    state.front = front;
    state.yaw = yaw;
    state.pitch = pitch;
    state.onGround = onGround;
    state.flyMode = flyMode;
    state.jumpPressedLastTick = m_jumpPressedLastTick;
    state.timeSinceGrounded = m_timeSinceGrounded;
    state.jumpBufferTimer = m_jumpBufferTimer;
    state.stepCooldownTimer = m_stepCooldownTimer;
    return state;
}

void Player::restoreSimulationState(const SimulationState &state) noexcept {
    position = state.position;
    velocity = state.velocity;
    front = state.front;
    yaw = static_cast<double>(NormalizeYawDegrees(static_cast<float>(state.yaw)));
    pitch = glm::clamp(state.pitch, -89.0f, 89.0f);
    onGround = state.onGround;
    flyMode = m_flyModeAllowed && state.flyMode;
    m_jumpPressedLastTick = state.jumpPressedLastTick;
    m_timeSinceGrounded = state.timeSinceGrounded;
    m_jumpBufferTimer = state.jumpBufferTimer;
    m_stepCooldownTimer = state.stepCooldownTimer;

    camera.updateRotation(static_cast<float>(yaw), pitch);
    front = camera.front;
    syncCameraToBody();
}

Player::PresentationState Player::capturePresentationState() const noexcept {
    PresentationState state;
    state.currentFov = currentFov;
    state.stepUpVisualOffset = m_stepUpVisualOffset;
    return state;
}

void Player::restorePresentationState(const PresentationState &state) noexcept {
    currentFov = state.currentFov;
    m_stepUpVisualOffset = std::max(0.0f, state.stepUpVisualOffset);
    syncCameraToBody();
}

void Player::setFlyModeAllowed(bool allowed) noexcept {
    if (m_flyModeAllowed == allowed) {
        return;
    }
    m_flyModeAllowed = allowed;
    if (!m_flyModeAllowed) {
        setFlyModeEnabled(false);
    }
}

void Player::setFlyModeEnabled(bool enabled) noexcept {
    const bool desired = m_flyModeAllowed && enabled;
    if (flyMode == desired) {
        return;
    }
    flyMode = desired;
    velocity = glm::vec3(0.0f);
    onGround = false;
    m_jumpPressedLastTick = false;
    m_timeSinceGrounded = 0.0f;
    m_jumpBufferTimer = 0.0f;
    m_stepCooldownTimer = 0.0f;
}

void Player::setTreatMissingCollisionAsSolid(bool enabled) noexcept {
    m_treatMissingCollisionAsSolid = enabled;
}

void Player::simulateFromNetworkInput(
    const NetworkInputState &input,
    double deltaTime,
    bool updateFov,
    bool disableAirControlWhenAirborne,
    const GrappleConstraintState *grappleConstraint
) {
    simulateMovement(
        input,
        static_cast<float>(deltaTime),
        updateFov,
        disableAirControlWhenAirborne,
        grappleConstraint
    );
}

void Player::simulateMovement(
    const NetworkInputState &input,
    float dt,
    bool updateFov,
    bool disableAirControlWhenAirborne,
    const GrappleConstraintState *grappleConstraint
) {
    const auto &movement = movementSettings();

    yaw = static_cast<double>(NormalizeYawDegrees(input.yaw));
    pitch = glm::clamp(input.pitch, -89.0f, 89.0f);
    camera.updateRotation(static_cast<float>(yaw), pitch);
    front = camera.front;

    const uint8_t flags = input.flags;
    const bool sprint = (flags & kPlayerInputFlagSprint) != 0;

    if (updateFov) {
        const float targetFov = sprint ? runningFov * runningFovMultiplier : walkFov;
        const float fovSmoothSpeed = 20.0f;
        currentFov += (targetFov - currentFov) * fovSmoothSpeed * dt;
    }

    Shared::Movement::State state;
    state.position = position;
    state.velocity = velocity;
    state.onGround = onGround;
    state.flyMode = flyMode;
    state.jumpPressedLastTick = m_jumpPressedLastTick;
    state.timeSinceGrounded = m_timeSinceGrounded;
    state.jumpBufferTimer = m_jumpBufferTimer;
    state.stepCooldownTimer = m_stepCooldownTimer;
    if (!state.flyMode && grappleConstraint != nullptr && grappleConstraint->active) {
        Shared::Grapple::ApplyRopeConstraint(
            grappleConstraint->anchor,
            grappleConstraint->ropeLength,
            state.position,
            state.velocity,
            [this](const glm::vec3 &testPos) { return checkCollision(testPos); }
        );
    }

    Shared::Movement::InputState simInput;
    simInput.moveX = input.moveX;
    simInput.moveZ = input.moveZ;
    simInput.flags = input.flags;
    simInput.flyMode = input.flyMode;

    Shared::Movement::Options simOptions;
    simOptions.allowFlyMode = m_flyModeAllowed;
    simOptions.allowStepUp = true;
    simOptions.requireSprintForStepUp = true;
    simOptions.disableAirControlWhenAirborne =
        disableAirControlWhenAirborne || (grappleConstraint != nullptr && grappleConstraint->active);

    float steppedHeight = 0.0f;
    Shared::Movement::Simulate(
        state,
        simInput,
        dt,
        movement,
        simOptions,
        [this](const glm::vec3 &testPos) { return checkCollision(testPos); },
        &steppedHeight
    );

    position = state.position;
    velocity = state.velocity;
    onGround = state.onGround;
    flyMode = state.flyMode;
    m_jumpPressedLastTick = state.jumpPressedLastTick;
    m_timeSinceGrounded = state.timeSinceGrounded;
    m_jumpBufferTimer = state.jumpBufferTimer;
    m_stepCooldownTimer = state.stepCooldownTimer;

    if (steppedHeight > 0.0f) {
        const float visualStep = std::min(steppedHeight * m_stepUpVisualScale, m_stepUpOffsetMax);
        m_stepUpVisualOffset = std::max(m_stepUpVisualOffset, visualStep);
    } else if (!onGround && velocity.y < 0.0f) {
        const float keep = std::exp(-18.0f * dt);
        m_stepUpVisualOffset *= std::clamp(keep, 0.0f, 1.0f);
        if (m_stepUpVisualOffset < 1e-4f) {
            m_stepUpVisualOffset = 0.0f;
        }
    } else {
        decayStepUpOffset(dt);
    }

    syncCameraToBody();
}



void Player::processMouse(bool dbgCam, double xpos, double ypos) noexcept {
    if (dbgCam) {
        return;
    }

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
        return;
    }

    double xoffset = xpos - lastX;
    double yoffset = ypos - lastY;
    lastX = xpos;
    lastY = ypos;

    xoffset *= mouseSensitivity;
    yoffset *= mouseSensitivity;

    yaw = static_cast<double>(NormalizeYawDegrees(static_cast<float>(yaw + xoffset)));
    pitch -= static_cast<float>(yoffset);
    pitch = glm::clamp(pitch, -89.0f, 89.0f);

    camera.updateRotation(yaw, pitch);
    front = camera.front;
    updateModelMatrix();
}

bool Player::isGrounded() const noexcept {
    return onGround;
}

glm::mat4 Player::getViewMatrix() const noexcept {
    return camera.getViewMatrix();
}
