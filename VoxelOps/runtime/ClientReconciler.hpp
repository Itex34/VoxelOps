#pragma once

#include <cstdint>

#include <glm/vec3.hpp>

struct Runtime;

class ClientReconciler {
public:
    struct ServerSnapshot {
        uint32_t serverTick = 0;
        uint32_t ackedInputTick = 0;
        glm::vec3 position{ 0.0f };
        glm::vec3 velocity{ 0.0f };
        bool onGround = false;
        bool flyMode = false;
        bool allowFlyMode = false;
        bool alive = true;
        float respawnSeconds = 0.0f;
        bool jumpPressedLastTick = false;
        float timeSinceGrounded = 0.0f;
        float jumpBufferTimer = 0.0f;
    };

    bool Apply(Runtime& runtime, const ServerSnapshot& snapshot);
};
