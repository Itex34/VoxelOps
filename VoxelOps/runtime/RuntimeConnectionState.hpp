#pragma once

#include "../../Shared/runtime/Paths.hpp"

#include <array>
#include <string>

struct RuntimeConnectionState {
    double nextReconnectAttemptTime = 0.0;
    double reconnectBackoffSeconds = 1.0;
    std::string lastConnectionStatus = "disconnected";
    std::array<char, 128> pendingServerEndpointInput{};
    std::array<char, kMaxConnectUsernameChars + 1> pendingUsernameInput{};
    bool wasEndpointPasteShortcutPressed = false;
    std::string usernamePromptError;
};
