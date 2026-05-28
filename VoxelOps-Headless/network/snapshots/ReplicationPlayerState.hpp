#pragma once

#include "../../../Shared/player/PlayerID.hpp"

#include <chrono>
#include <cstdint>

#include <glm/vec3.hpp>

struct ReplicationPlayerState {
    PlayerID id = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool onGround = false;
    bool flyMode = false;
    bool allowFlyMode = false;
    uint16_t weaponId = 0;
    float health = 0.0f;
    bool isAlive = false;
    std::chrono::steady_clock::time_point respawnAt{};
    bool jumpPressedLastTick = false;
    float timeSinceGrounded = 0.0f;
    float jumpBufferTimer = 0.0f;
    uint32_t lastProcessedInputTick = 0;
};
