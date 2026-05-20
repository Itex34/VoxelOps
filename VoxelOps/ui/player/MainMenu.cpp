#include "MainMenu.hpp"

#include "../../application/AppHelpers.hpp"
#include "../../data/GameData.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

using namespace AppHelpers;

namespace {
    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }
} // namespace

void MainMenu::draw(Runtime &runtime, const MainMenuContext &ctx) {
    if (!runtime.ui.debugUi || runtime.network.clientNet.IsConnected()) {
        return;
    }

    GameData::cursorEnabled = true;

    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 windowSize(460.0f, 0.0f);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f)
    );

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

    if (!ImGui::Begin("Connect", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Server (host:port)");
    const float pasteButtonWidth =
        ImGui::CalcTextSize("Paste").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
    const float endpointFieldWidth =
        ImGui::GetContentRegionAvail().x - pasteButtonWidth - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(endpointFieldWidth > 60.0f ? endpointFieldWidth : -1.0f);
    const ClientNetwork::ConnectionState connState = runtime.network.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);
    const auto pasteEndpointFromClipboard = [&]() -> bool {
        if (ctx.window == nullptr) {
            return false;
        }
        char *clipboardText = SDL_GetClipboardText();
        if (clipboardText == nullptr || clipboardText[0] == '\0') {
            if (clipboardText != nullptr) {
                SDL_free(clipboardText);
            }
            return false;
        }
        const std::string endpoint = TrimAscii(clipboardText);
        SDL_free(clipboardText);
        if (endpoint.empty()) {
            return false;
        }
        std::memset(
            runtime.app.connection.pendingServerEndpointInput.data(),
            0,
            runtime.app.connection.pendingServerEndpointInput.size()
        );
        const size_t copyLen =
            std::min(endpoint.size(), runtime.app.connection.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.app.connection.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        return true;
    };

    bool submit = false;
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText(
            "##server_endpoint_input",
            runtime.app.connection.pendingServerEndpointInput.data(),
            runtime.app.connection.pendingServerEndpointInput.size(),
            ImGuiInputTextFlags_EnterReturnsTrue
        )) {
        submit = true;
    }
    const bool endpointFieldActive = ImGui::IsItemActive();
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    bool pasteShortcutPressed = false;
    if (endpointFieldActive && !isConnecting) {
        const bool ctrlDown =
            IsScancodeDown(SDL_SCANCODE_LCTRL) || IsScancodeDown(SDL_SCANCODE_RCTRL) ||
            IsScancodeDown(SDL_SCANCODE_LGUI) || IsScancodeDown(SDL_SCANCODE_RGUI);
        const bool shiftDown =
            IsScancodeDown(SDL_SCANCODE_LSHIFT) || IsScancodeDown(SDL_SCANCODE_RSHIFT);
        const bool pasteCtrlV = ctrlDown && IsScancodeDown(SDL_SCANCODE_V);
        const bool pasteShiftInsert = shiftDown && IsScancodeDown(SDL_SCANCODE_INSERT);
        pasteShortcutPressed = pasteCtrlV || pasteShiftInsert;
        if (pasteShortcutPressed && !runtime.app.connection.wasEndpointPasteShortcutPressed) {
            if (!pasteEndpointFromClipboard()) {
                runtime.app.connection.usernamePromptError = "Clipboard is empty.";
            }
        }
    }
    runtime.app.connection.wasEndpointPasteShortcutPressed = pasteShortcutPressed;

    ImGui::SameLine();
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Paste")) {
        if (!pasteEndpointFromClipboard()) {
            runtime.app.connection.usernamePromptError = "Clipboard is empty.";
        }
    }
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Enter username");
    ImGui::SetNextItemWidth(-1.0f);
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText(
            "##username_input",
            runtime.app.connection.pendingUsernameInput.data(),
            runtime.app.connection.pendingUsernameInput.size(),
            ImGuiInputTextFlags_EnterReturnsTrue
        )) {
        submit = true;
    }
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    if (!isConnecting) {
        if (ImGui::Button("Connect")) {
            submit = true;
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Connecting...");
        ImGui::EndDisabled();
    }

    if (submit) {
        std::string desiredEndpoint = runtime.app.connection.pendingServerEndpointInput.data();
        std::string parsedIp;
        uint16_t parsedPort = 0;
        if (!ParseServerEndpoint(desiredEndpoint, parsedIp, parsedPort)) {
            runtime.app.connection.usernamePromptError =
                "Server must be host:port (example: 127.0.0.1:27015).";
            ImGui::End();
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
        while (end > begin &&
               std::isspace(static_cast<unsigned char>(desiredUsername[end - 1])) != 0) {
            --end;
        }
        desiredUsername = desiredUsername.substr(begin, end - begin);

        if (desiredUsername.empty()) {
            runtime.app.connection.usernamePromptError = "Please enter a username.";
        } else {
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
            std::memset(
                runtime.app.connection.pendingServerEndpointInput.data(),
                0,
                runtime.app.connection.pendingServerEndpointInput.size()
            );
            const size_t endpointCopyLen =
                std::min(endpoint.size(), runtime.app.connection.pendingServerEndpointInput.size() - 1);
            std::memcpy(
                runtime.app.connection.pendingServerEndpointInput.data(),
                endpoint.data(),
                endpointCopyLen
            );

            if (ctx.requestedUsername) {
                *ctx.requestedUsername = desiredUsername;
            }
            std::memset(
                runtime.app.connection.pendingUsernameInput.data(),
                0,
                runtime.app.connection.pendingUsernameInput.size()
            );
            std::memcpy(
                runtime.app.connection.pendingUsernameInput.data(),
                desiredUsername.data(),
                desiredUsername.size()
            );

            runtime.app.connection.usernamePromptError.clear();
            if (ctx.connectionHost == nullptr || !ctx.connectionHost->beginConnectionAttempt(runtime)) {
                runtime.app.connection.usernamePromptError =
                    "Failed to start connection. Check server reachability and retry.";
            }
            runtime.app.connection.nextReconnectAttemptTime = GetTimeSeconds() + 1.0;
        }
    }

    if (!runtime.app.connection.usernamePromptError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", runtime.app.connection.usernamePromptError.c_str()
        );
    }

    ImGui::Spacing();
    ImGui::TextWrapped("If that username is already taken, enter a different username and retry.");
    ImGui::Text("Status: %s", runtime.network.clientNet.GetConnectionStatusText().c_str());

    ImGui::End();
    if (ctx.windowHost != nullptr) {
        ctx.windowHost->applyMouseInputModes();
    }
}
