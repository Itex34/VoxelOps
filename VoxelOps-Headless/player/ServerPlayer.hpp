#pragma once
#include <cstdint>
#include <chrono>
#include <glm/vec3.hpp>
#include <list>
#include <memory>
#include <deque>
#include <limits>

#include "../../Shared/network/Packets.hpp"
#include "../../Shared/gun/GunType.hpp"

using PlayerID = uint64_t;
using Clock = std::chrono::steady_clock;

struct ConnectionHandle {
    // Replace with your session type. Must provide a thread-safe send() method.
    // Example placeholder:
    int socketFd = -1;
    // Example API we'll call in PlayerManager: bool send(const void* data, size_t size);
    // Or provide a method pointer/lambda to send bytes.
};

struct ServerPlayer {
    PlayerID id = 0;
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool onGround = false;

    float height = 2.56f;
    float radius = 0.3f;
    float health = 100.0f;
    float maxHealth = 100.0f;
    bool isAlive = true;
    Clock::time_point respawnAt{};
    bool pendingRespawnRequest = false;

    std::shared_ptr<ConnectionHandle> conn; // nullable
    Clock::time_point lastHeartbeat = Clock::now();
    Clock::time_point lastInputReceived = Clock::now();
    std::deque<PlayerInput> pendingInputs;
    uint32_t lastProcessedInputSequence = std::numeric_limits<uint32_t>::max();
    uint8_t activeInputFlags = 0;
    bool flyMode = false;
    bool allowFlyMode = false;
    uint16_t equippedWeaponId = ToWeaponId(GunType::Pistol);
    float moveX = 0.0f;
    float moveZ = 0.0f;
    bool jumpPressedLastTick = false;
    float timeSinceGrounded = 0.0f;
    float jumpBufferTimer = 0.0f;

    // For fast O(1) removal from order list (set by PlayerManager)
    std::list<PlayerID>::iterator orderIt;
};
