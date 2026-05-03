#pragma once

#include "ClientInputIntent.hpp"
#include "../runtime/Runtime.hpp"

struct SDL_Window;

class ClientInputSystem {
public:
    ClientInputIntent captureIntent(Runtime &runtime, SDL_Window *window);
    void updatePrediction(Runtime &runtime, const ClientInputIntent &intent, double deltaTime);

private:
    static NetworkInputState captureFromKeyboard(
        const Player &player, SDL_Window *window, bool allowGameplayInput
    );

    bool m_f8PressedLast = false;
};
