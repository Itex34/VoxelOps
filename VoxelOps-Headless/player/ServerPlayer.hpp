#pragma once
#include <cstdint>
#include <chrono>
#include <glm/vec3.hpp>
#include <list>
#include <memory>

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

    float height = 1.8f;
    float radius = 0.3f;

    std::shared_ptr<ConnectionHandle> conn; // nullable
    Clock::time_point lastHeartbeat = Clock::now();

    // For fast O(1) removal from order list (set by PlayerManager)
    std::list<PlayerID>::iterator orderIt;
};
