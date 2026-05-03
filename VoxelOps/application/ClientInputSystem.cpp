#include "ClientInputSystem.hpp"

#include "AppHelpers.hpp"

#include "../data/GameData.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <iostream>

namespace {

bool IsScancodeDown(SDL_Scancode scancode) {
    int keyCount = 0;
    const bool *keys = SDL_GetKeyboardState(&keyCount);
    return keys != nullptr && scancode < keyCount && keys[scancode];
}

} // namespace

NetworkInputState ClientInputSystem::captureFromKeyboard(
    const Player &player, SDL_Window *window, bool allowGameplayInput
) {
    (void)window;

    const bool keyW = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_W);
    const bool keyS = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_S);
    const bool keyA = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_A);
    const bool keyD = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_D);
    const bool keyShift = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_LSHIFT);
    const bool keySpace = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_SPACE);
    const bool keyCtrl = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_LCTRL);

    const glm::vec2 localMove(
        (keyD ? 1.0f : 0.0f) - (keyA ? 1.0f : 0.0f),
        (keyW ? 1.0f : 0.0f) - (keyS ? 1.0f : 0.0f)
    );
    glm::vec2 localMoveNormalized = localMove;
    if (glm::length(localMoveNormalized) > 1.0f) {
        localMoveNormalized = glm::normalize(localMoveNormalized);
    }

    const float yawDegrees = player.getYawDegrees();
    const float yawRad = glm::radians(yawDegrees);
    const glm::vec2 forward2D(std::cos(yawRad), std::sin(yawRad));
    const glm::vec2 right2D(-forward2D.y, forward2D.x);
    const glm::vec2 worldMove = right2D * localMoveNormalized.x + forward2D * localMoveNormalized.y;

    NetworkInputState input;
    input.moveX = worldMove.x;
    input.moveZ = worldMove.y;
    input.yaw = AppHelpers::NormalizeYawDegrees(yawDegrees);
    input.pitch = player.getPitchDegrees();
    input.flyMode = player.flyMode;
    input.flags = 0;
    if (keyW)
        input.flags |= kPlayerInputFlagForward;
    if (keyS)
        input.flags |= kPlayerInputFlagBackward;
    if (keyA)
        input.flags |= kPlayerInputFlagLeft;
    if (keyD)
        input.flags |= kPlayerInputFlagRight;
    if (keySpace)
        input.flags |= kPlayerInputFlagJump;
    if (keyShift)
        input.flags |= kPlayerInputFlagSprint;
    if (player.flyMode && keySpace)
        input.flags |= kPlayerInputFlagFlyUp;
    if (player.flyMode && keyCtrl)
        input.flags |= kPlayerInputFlagFlyDown;

    return input;
}

ClientInputIntent ClientInputSystem::captureIntent(Runtime &runtime, SDL_Window *window) {
    ClientInputIntent intent{};
    if (!runtime.gameplay.player) {
        return intent;
    }

    const bool allowGameplayInput =
        GameData::gameplayInputEnabled && !AppHelpers::IsImGuiTextInputActive();
    intent.gameplayInputEnabled = allowGameplayInput;

    const bool f8Pressed = allowGameplayInput && IsScancodeDown(SDL_SCANCODE_F8);
    if (runtime.gameplay.player->isFlyModeAllowed() && f8Pressed && !m_f8PressedLast) {
        runtime.gameplay.player->setFlyModeEnabled(!runtime.gameplay.player->flyMode);
        std::cout << (runtime.gameplay.player->flyMode ? "Fly mode ON\n" : "Fly mode OFF\n");
    } else if (!runtime.gameplay.player->isFlyModeAllowed()) {
        runtime.gameplay.player->setFlyModeEnabled(false);
    }
    m_f8PressedLast = f8Pressed;

    const NetworkInputState input = captureFromKeyboard(*runtime.gameplay.player, window, allowGameplayInput);
    runtime.gameplay.player->setNetworkInputState(input);
    intent.networkInput = input;
    return intent;
}

void ClientInputSystem::updatePrediction(
    Runtime &runtime, const ClientInputIntent &intent, double deltaTime
) {
    if (!runtime.gameplay.player) {
        return;
    }

    runtime.gameplay.player->setNetworkInputState(intent.networkInput);
    if (deltaTime > 0.0) {
        runtime.gameplay.player->simulateFromNetworkInput(intent.networkInput, deltaTime, true);
    }
}
