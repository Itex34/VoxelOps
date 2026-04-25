#include "graphics/Vulkan/core/Player.hpp"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include <algorithm>

Player::Player(glm::vec3 startPos) : playerCamera(startPos) {}

void Player::update(SDL_Window *window) {
    if (!window) {
        return;
    }

    const uint64_t nowNs = SDL_GetTicksNS();
    float deltaTimeSeconds = 0.016f;
    if (lastUpdateNs != 0 && nowNs > lastUpdateNs) {
        deltaTimeSeconds =
            static_cast<float>(static_cast<double>(nowNs - lastUpdateNs) / 1'000'000'000.0);
    }
    lastUpdateNs = nowNs;
    deltaTimeSeconds = std::clamp(deltaTimeSeconds, 0.0f, 0.1f);

    processKeyboardInput(window, deltaTimeSeconds);

    const bool isRelativeMouseMode = SDL_GetWindowRelativeMouseMode(window);
    if (!isRelativeMouseMode) {
        mouseWasRelativeLastFrame = false;
        return;
    }

    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    SDL_GetRelativeMouseState(&mouseDeltaX, &mouseDeltaY);

    if (!mouseWasRelativeLastFrame) {
        // drop first relative delta right after relock to avoid camera jump.
        mouseWasRelativeLastFrame = true;
        return;
    }

    processMouseInput(mouseDeltaX, mouseDeltaY);
}

void Player::processMouseInput(float deltaX, float deltaY) noexcept {
    const float newYaw = playerCamera.getYaw() + (deltaX * mouseSensitivity);
    const float newPitch = playerCamera.getPitch() - (deltaY * mouseSensitivity);
    playerCamera.updateRotation(newYaw, newPitch);
}

void Player::processKeyboardInput(SDL_Window *window, float deltaTimeSeconds) {
    int keyCount = 0;
    const bool *keys = SDL_GetKeyboardState(&keyCount);
    if (!keys) {
        return;
    }

    float currentSpeed = moveSpeed * deltaTimeSeconds;
    if ((SDL_SCANCODE_LSHIFT < keyCount && keys[SDL_SCANCODE_LSHIFT]) ||
        (SDL_SCANCODE_RSHIFT < keyCount && keys[SDL_SCANCODE_RSHIFT])) {
        currentSpeed *= sprintMultiplier;
    }

    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::cross(playerCamera.front, worldUp);
    if (glm::length(right) > 0.0001f) {
        right = glm::normalize(right);
    } else {
        right = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    if (SDL_SCANCODE_W < keyCount && keys[SDL_SCANCODE_W]) {
        playerCamera.position += playerCamera.XZfront * currentSpeed;
    }
    if (SDL_SCANCODE_S < keyCount && keys[SDL_SCANCODE_S]) {
        playerCamera.position -= playerCamera.XZfront * currentSpeed;
    }
    if (SDL_SCANCODE_A < keyCount && keys[SDL_SCANCODE_A]) {
        playerCamera.position -= right * currentSpeed;
    }
    if (SDL_SCANCODE_D < keyCount && keys[SDL_SCANCODE_D]) {
        playerCamera.position += right * currentSpeed;
    }
    if (SDL_SCANCODE_SPACE < keyCount && keys[SDL_SCANCODE_SPACE]) {
        playerCamera.position.y += currentSpeed;
    }
    if ((SDL_SCANCODE_LCTRL < keyCount && keys[SDL_SCANCODE_LCTRL]) ||
        (SDL_SCANCODE_RCTRL < keyCount && keys[SDL_SCANCODE_RCTRL]) ||
        (SDL_SCANCODE_C < keyCount && keys[SDL_SCANCODE_C])) {
        playerCamera.position.y -= currentSpeed;
    }

    const bool escapePressedNow = (SDL_SCANCODE_ESCAPE < keyCount) && keys[SDL_SCANCODE_ESCAPE];
    if (escapePressedNow && !escapePressedLastFrame) {
        if (!SDL_SetWindowRelativeMouseMode(window, false)) {
            SDL_Log("Failed to disable relative mouse mode: %s", SDL_GetError());
        }
    }
    escapePressedLastFrame = escapePressedNow;
}
