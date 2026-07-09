#include "MainMenu.hpp"

#include "../../application/AppHelpers.hpp"
#include "../../data/GameData.hpp"
#include "../widgets/UIContext.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

using namespace AppHelpers;

namespace {
    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }

    template <size_t N>
    void CopyStringToArray(std::string_view text, std::array<char, N> &target) {
        std::memset(target.data(), 0, target.size());
        const size_t copyLen = std::min(text.size(), target.size() - 1);
        std::memcpy(target.data(), text.data(), copyLen);
    }

    std::string Ellipsize(std::string text, size_t maxChars) {
        if (text.size() <= maxChars) {
            return text;
        }
        if (maxChars <= 3) {
            return text.substr(0, maxChars);
        }
        return text.substr(0, maxChars - 3) + "...";
    }
} // namespace

MainMenu::MainMenu() = default;
MainMenu::~MainMenu() = default;

void MainMenu::draw(Runtime &runtime, const MainMenuContext &ctx) {
    if (runtime.network.clientNet.IsConnected()) {
        hide();
        return;
    }

    GameData::cursorEnabled = true;

    if (runtime.ui.nativeUi && runtime.ui.nativeUi->hasBackendRenderer()) {
        drawNative(runtime, ctx);
        return;
    }
}

void MainMenu::drawNative(Runtime &runtime, const MainMenuContext &ctx) {

    UIContext &ui = runtime.ui.nativeUi->context();
    const glm::vec2 screen = ui.screenSize();
    const float panelWidth = std::min(500.0f, std::max(360.0f, screen.x - 48.0f));
    const float panelHeight = 292.0f;
    const float x = (screen.x - panelWidth) * 0.5f;
    const float y = std::max(36.0f, (screen.y - panelHeight) * 0.5f);

    const ClientNetwork::ConnectionState connState = runtime.network.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);

    ui.panel(Rect{x, y, panelWidth, panelHeight}, Color{0.035f, 0.038f, 0.043f, 0.92f});
    ui.panel(Rect{x, y, panelWidth, 2.0f}, Color{1.0f, 0.63f, 0.22f, 1.0f});
    ui.label("Connect", glm::vec2(x + 24.0f, y + 20.0f), Color{0.98f, 0.98f, 0.98f, 1.0f});
    ui.label(
        "Server (host:port)",
        glm::vec2(x + 24.0f, y + 62.0f),
        Color{0.74f, 0.76f, 0.79f, 1.0f}
    );

    bool submit = false;
    const float fieldHeight = 38.0f;
    const float pasteWidth = fieldHeight;
    const float endpointY = y + 84.0f;
    const float fieldWidth = panelWidth - pasteWidth - 58.0f;
    submit = ui.inputText(
        "main_menu_endpoint",
        runtime.app.connection.pendingServerEndpointInput.data(),
        runtime.app.connection.pendingServerEndpointInput.size(),
        Rect{x + 24.0f, endpointY, fieldWidth, fieldHeight},
        !isConnecting
    ) || submit;

    const TextureHandle pasteIcon = runtime.ui.nativeUi->icon(NativeUiIcon::Paste);
    const TextureHandle connectIcon = runtime.ui.nativeUi->icon(NativeUiIcon::Connect);

    if (ui.iconButton(
            "paste_endpoint",
            pasteIcon,
            Rect{x + 34.0f + fieldWidth, endpointY, pasteWidth, fieldHeight},
            !isConnecting
        )) {
        handlePaste(runtime);
    }

    ui.label("Username", glm::vec2(x + 24.0f, y + 136.0f), Color{0.74f, 0.76f, 0.79f, 1.0f});
    submit = ui.inputText(
        "main_menu_username",
        runtime.app.connection.pendingUsernameInput.data(),
        runtime.app.connection.pendingUsernameInput.size(),
        Rect{x + 24.0f, y + 158.0f, panelWidth - 48.0f, fieldHeight},
        !isConnecting
    ) || submit;

    if (ui.iconButton(
            "connect",
            connectIcon,
            Rect{x + 24.0f, y + 214.0f, panelWidth - 48.0f, 40.0f},
            !isConnecting
        )) {
        submit = true;
    }

    if (submit && !isConnecting) {
        handleSubmit(runtime, ctx);
    }

    if (!runtime.app.connection.usernamePromptError.empty()) {
        ui.label(
            Ellipsize(runtime.app.connection.usernamePromptError, 58),
            glm::vec2(x + 24.0f, y + 264.0f),
            Color{1.0f, 0.42f, 0.34f, 1.0f}
        );
    } else {
        const std::string status =
            "Status: " + Ellipsize(runtime.network.clientNet.GetConnectionStatusText(), 44);
        ui.label(status, glm::vec2(x + 24.0f, y + 264.0f), Color{0.76f, 0.78f, 0.80f, 1.0f});
    }

    if (ctx.windowHost != nullptr) {
        ctx.windowHost->applyMouseInputModes();
    }
}

void MainMenu::handlePaste(Runtime &runtime) {
    char *clipboardText = SDL_GetClipboardText();
    if (clipboardText == nullptr || clipboardText[0] == '\0') {
        if (clipboardText != nullptr) {
            SDL_free(clipboardText);
        }
        runtime.app.connection.usernamePromptError = "Clipboard is empty.";
        return;
    }

    const std::string endpoint = TrimAscii(clipboardText);
    SDL_free(clipboardText);
    if (endpoint.empty()) {
        runtime.app.connection.usernamePromptError = "Clipboard is empty.";
        return;
    }

    CopyStringToArray(endpoint, runtime.app.connection.pendingServerEndpointInput);
}

void MainMenu::handleSubmit(Runtime &runtime, const MainMenuContext &ctx) {
    std::string desiredEndpoint = runtime.app.connection.pendingServerEndpointInput.data();
    std::string parsedIp;
    uint16_t parsedPort = 0;
    if (!ParseServerEndpoint(desiredEndpoint, parsedIp, parsedPort)) {
        runtime.app.connection.usernamePromptError =
            "Server must be host:port (example: 127.0.0.1:27015).";
        if (ctx.windowHost != nullptr) {
            ctx.windowHost->applyMouseInputModes();
        }
        return;
    }

    std::string desiredUsername = runtime.app.connection.pendingUsernameInput.data();
    size_t begin = 0;
    while (begin < desiredUsername.size() &&
           std::isspace(static_cast<unsigned char>(desiredUsername[begin])) != 0) {
        ++begin;
    }
    size_t end = desiredUsername.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(desiredUsername[end - 1])) != 0) {
        --end;
    }
    desiredUsername = desiredUsername.substr(begin, end - begin);

    if (desiredUsername.empty()) {
        runtime.app.connection.usernamePromptError = "Please enter a username.";
        if (ctx.windowHost != nullptr) {
            ctx.windowHost->applyMouseInputModes();
        }
        return;
    }

    if (desiredUsername.size() > kMaxConnectUsernameChars) {
        desiredUsername.resize(kMaxConnectUsernameChars);
    }

    if (ctx.serverIp) {
        *ctx.serverIp = parsedIp;
    }
    if (ctx.serverPort) {
        *ctx.serverPort = parsedPort;
    }

    const std::string endpoint = parsedIp + ":" + std::to_string(parsedPort);
    CopyStringToArray(endpoint, runtime.app.connection.pendingServerEndpointInput);

    if (ctx.requestedUsername) {
        *ctx.requestedUsername = desiredUsername;
    }
    CopyStringToArray(desiredUsername, runtime.app.connection.pendingUsernameInput);

    runtime.app.connection.usernamePromptError.clear();
    if (ctx.connectionHost == nullptr || !ctx.connectionHost->beginConnectionAttempt(runtime)) {
        runtime.app.connection.usernamePromptError =
            "Failed to start connection. Check server reachability and retry.";
    }
    runtime.app.connection.nextReconnectAttemptTime = GetTimeSeconds() + 1.0;

    if (ctx.windowHost != nullptr) {
        ctx.windowHost->applyMouseInputModes();
    }
}


void MainMenu::hide() {}
