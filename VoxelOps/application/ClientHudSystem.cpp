#include "ClientHudSystem.hpp"

#include "AppHelpers.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
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

void ClientHudSystem::draw(Runtime &runtime, const ClientHudSystemContext &ctx) {
    drawConnectionPrompt(runtime, ctx);
    drawScoreboard(runtime);
    drawPingCounter(runtime);
    drawKillFeed(runtime);
    drawPlayerHud(runtime);
    drawDeathOverlay(runtime);
}

void ClientHudSystem::drawConnectionPrompt(Runtime &runtime, const ClientHudSystemContext &ctx) {
    if (!runtime.debugUi || runtime.clientNet.IsConnected()) {
        return;
    }

    GameData::cursorEnabled = true;

    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 windowSize(460.0f, 0.0f);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

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
    const ClientNetwork::ConnectionState connState = runtime.clientNet.GetConnectionState();
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
        std::memset(runtime.connection.pendingServerEndpointInput.data(), 0,
                    runtime.connection.pendingServerEndpointInput.size());
        const size_t copyLen =
            std::min(endpoint.size(), runtime.connection.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.connection.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        return true;
    };

    bool submit = false;
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText("##server_endpoint_input", runtime.connection.pendingServerEndpointInput.data(),
                         runtime.connection.pendingServerEndpointInput.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
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
        if (pasteShortcutPressed && !runtime.connection.wasEndpointPasteShortcutPressed) {
            if (!pasteEndpointFromClipboard()) {
                runtime.connection.usernamePromptError = "Clipboard is empty.";
            }
        }
    }
    runtime.connection.wasEndpointPasteShortcutPressed = pasteShortcutPressed;

    ImGui::SameLine();
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Paste")) {
        if (!pasteEndpointFromClipboard()) {
            runtime.connection.usernamePromptError = "Clipboard is empty.";
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
    if (ImGui::InputText("##username_input", runtime.connection.pendingUsernameInput.data(),
                         runtime.connection.pendingUsernameInput.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
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
        std::string desiredEndpoint = runtime.connection.pendingServerEndpointInput.data();
        std::string parsedIp;
        uint16_t parsedPort = 0;
        if (!ParseServerEndpoint(desiredEndpoint, parsedIp, parsedPort)) {
            runtime.connection.usernamePromptError =
                "Server must be host:port (example: 127.0.0.1:27015).";
            ImGui::End();
            if (ctx.applyMouseInputModes) {
                ctx.applyMouseInputModes();
            }
            return;
        }

        std::string desiredUsername = runtime.connection.pendingUsernameInput.data();
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
            runtime.connection.usernamePromptError = "Please enter a username.";
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
            std::memset(runtime.connection.pendingServerEndpointInput.data(), 0,
                        runtime.connection.pendingServerEndpointInput.size());
            const size_t endpointCopyLen =
                std::min(endpoint.size(), runtime.connection.pendingServerEndpointInput.size() - 1);
            std::memcpy(runtime.connection.pendingServerEndpointInput.data(), endpoint.data(),
                        endpointCopyLen);

            if (ctx.requestedUsername) {
                *ctx.requestedUsername = desiredUsername;
            }
            std::memset(runtime.connection.pendingUsernameInput.data(), 0,
                        runtime.connection.pendingUsernameInput.size());
            std::memcpy(runtime.connection.pendingUsernameInput.data(), desiredUsername.data(),
                        desiredUsername.size());

            runtime.connection.usernamePromptError.clear();
            if (!ctx.beginConnectionAttempt || !ctx.beginConnectionAttempt(runtime)) {
                runtime.connection.usernamePromptError =
                    "Failed to start connection. Check server reachability and retry.";
            }
            runtime.connection.nextReconnectAttemptTime = GetTimeSeconds() + 1.0;
        }
    }

    if (!runtime.connection.usernamePromptError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                           runtime.connection.usernamePromptError.c_str());
    }

    ImGui::Spacing();
    ImGui::TextWrapped("If that username is already taken, enter a different username and retry.");
    ImGui::Text("Status: %s", runtime.clientNet.GetConnectionStatusText().c_str());

    ImGui::End();
    if (ctx.applyMouseInputModes) {
        ctx.applyMouseInputModes();
    }
}

void ClientHudSystem::drawKillFeed(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr || runtime.killFeedEntries.empty()) {
        return;
    }

    const double now = GetTimeSeconds();
    while (!runtime.killFeedEntries.empty() && runtime.killFeedEntries.back().expiresAt <= now) {
        runtime.killFeedEntries.pop_back();
    }
    if (runtime.killFeedEntries.empty()) {
        return;
    }

    const std::string localName = runtime.clientNet.GetAssignedUsername();
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    float y = 24.0f;

    for (const Runtime::KillFeedEntry &entry : runtime.killFeedEntries) {
        const std::string line = entry.killer + " [" +
                                 std::string(GunTypeName(static_cast<GunType>(entry.weaponId))) +
                                 "] " + entry.victim;
        const ImVec2 textSize = ImGui::CalcTextSize(line.c_str());
        const float x = io.DisplaySize.x - textSize.x - 24.0f;

        ImU32 textColor = IM_COL32(232, 232, 232, 255);
        if (!localName.empty() && entry.killer == localName) {
            textColor = IM_COL32(130, 255, 160, 255);
        } else if (!localName.empty() && entry.victim == localName) {
            textColor = IM_COL32(255, 120, 120, 255);
        }

        const ImVec2 bgMin(x - 8.0f, y - 3.0f);
        const ImVec2 bgMax(x + textSize.x + 8.0f, y + textSize.y + 3.0f);
        drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 125), 4.0f);
        drawList->AddText(ImVec2(x, y), textColor, line.c_str());
        y += textSize.y + 8.0f;
    }
}

void ClientHudSystem::drawScoreboard(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const bool showScoreboard = IsScancodeDown(SDL_SCANCODE_TAB) || runtime.matchEnded;
    if (!showScoreboard) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    const float panelWidth = 560.0f;
    const float rowHeight = ImGui::GetTextLineHeight() + 8.0f;
    const float headerHeight = 66.0f;
    const float tableHeaderHeight = rowHeight;
    const float panelHeight = headerHeight + tableHeaderHeight +
                              rowHeight * static_cast<float>(runtime.scoreboardEntries.size()) +
                              14.0f;
    const float x = (io.DisplaySize.x - panelWidth) * 0.5f;
    const float y = 72.0f;

    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + panelWidth, y + panelHeight),
                            IM_COL32(10, 10, 10, 215), 8.0f);

    const int clampedRemaining = std::max(0, runtime.matchRemainingSeconds);
    const int minutes = clampedRemaining / 60;
    const int seconds = clampedRemaining % 60;
    char timerLine[64]{};
    if (!runtime.matchStarted) {
        std::snprintf(timerLine, sizeof(timerLine), "Waiting for players");
    } else {
        std::snprintf(timerLine, sizeof(timerLine), "Time Left: %02d:%02d", minutes, seconds);
    }

    std::string title = "Deathmatch";
    if (runtime.matchEnded) {
        title = "Match Ended";
        if (!runtime.matchWinner.empty()) {
            title += " - Winner: ";
            title += runtime.matchWinner;
        }
    }

    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    drawList->AddText(ImVec2(x + (panelWidth - titleSize.x) * 0.5f, y + 12.0f),
                      IM_COL32(245, 245, 245, 255), title.c_str());
    const ImVec2 timerSize = ImGui::CalcTextSize(timerLine);
    drawList->AddText(ImVec2(x + (panelWidth - timerSize.x) * 0.5f, y + 34.0f),
                      IM_COL32(210, 210, 210, 255), timerLine);

    const float tableY = y + headerHeight;
    drawList->AddRectFilled(ImVec2(x + 8.0f, tableY),
                            ImVec2(x + panelWidth - 8.0f, tableY + tableHeaderHeight),
                            IM_COL32(32, 32, 32, 220), 4.0f);

    const float nameX = x + 24.0f;
    const float killsX = x + 360.0f;
    const float deathsX = x + 430.0f;
    const float pingX = x + 495.0f;
    drawList->AddText(ImVec2(nameX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "Player");
    drawList->AddText(ImVec2(killsX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "K");
    drawList->AddText(ImVec2(deathsX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "D");
    drawList->AddText(ImVec2(pingX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "Ping");

    const std::string localName = runtime.clientNet.GetAssignedUsername();
    float rowY = tableY + tableHeaderHeight;
    for (size_t i = 0; i < runtime.scoreboardEntries.size(); ++i) {
        const ClientNetwork::ScoreboardEntry &entry = runtime.scoreboardEntries[i];
        const bool oddRow = ((i % 2) != 0);
        if (oddRow) {
            drawList->AddRectFilled(ImVec2(x + 8.0f, rowY),
                                    ImVec2(x + panelWidth - 8.0f, rowY + rowHeight),
                                    IM_COL32(20, 20, 20, 145), 0.0f);
        }

        ImU32 nameColor = IM_COL32(230, 230, 230, 255);
        if (!localName.empty() && entry.username == localName) {
            nameColor = IM_COL32(130, 255, 160, 255);
        }
        drawList->AddText(ImVec2(nameX, rowY + 4.0f), nameColor, entry.username.c_str());
        drawList->AddText(ImVec2(killsX, rowY + 4.0f), IM_COL32(230, 230, 230, 255),
                          std::to_string(entry.kills).c_str());
        drawList->AddText(ImVec2(deathsX, rowY + 4.0f), IM_COL32(230, 230, 230, 255),
                          std::to_string(entry.deaths).c_str());
        const std::string pingText =
            (entry.pingMs >= 0) ? std::to_string(entry.pingMs) : std::string("--");
        drawList->AddText(ImVec2(pingX, rowY + 4.0f), IM_COL32(230, 230, 230, 255),
                          pingText.c_str());

        rowY += rowHeight;
    }
}

void ClientHudSystem::drawPingCounter(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const int pingMs = runtime.clientNet.GetPingMs();
    const std::string line =
        (pingMs >= 0) ? ("Ping: " + std::to_string(pingMs) + " ms") : "Ping: --";

    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    const float x = 24.0f;
    const float y = 24.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(line.c_str());

    ImU32 textColor = IM_COL32(232, 232, 232, 255);
    if (pingMs >= 150) {
        textColor = IM_COL32(255, 120, 120, 255);
    } else if (pingMs >= 80) {
        textColor = IM_COL32(255, 220, 120, 255);
    }

    const ImVec2 bgMin(x - 8.0f, y - 3.0f);
    const ImVec2 bgMax(x + textSize.x + 8.0f, y + textSize.y + 3.0f);
    drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 125), 4.0f);
    drawList->AddText(ImVec2(x, y), textColor, line.c_str());
}

void ClientHudSystem::drawPlayerHud(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr || !runtime.clientNet.IsConnected()) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *drawList = ImGui::GetForegroundDrawList();

    const float health = std::clamp(runtime.combat.localHealth, 0.0f, 100.0f);
    const float healthPct = health / 100.0f;
    const float healthBarWidth = 240.0f;
    const float healthBarHeight = 18.0f;
    const float healthBarX = 24.0f;
    const float healthBarY = io.DisplaySize.y - 102.0f;
    const ImVec2 healthMin(healthBarX, healthBarY);
    const ImVec2 healthMax(healthBarX + healthBarWidth, healthBarY + healthBarHeight);

    drawList->AddRectFilled(healthMin, healthMax, IM_COL32(0, 0, 0, 140), 4.0f);
    const ImU32 healthColor = runtime.combat.localPlayerAlive ? IM_COL32(120, 220, 120, 255)
                                                               : IM_COL32(220, 80, 80, 255);
    drawList->AddRectFilled(
        healthMin, ImVec2(healthBarX + (healthBarWidth * healthPct), healthBarY + healthBarHeight),
        healthColor, 4.0f);
    drawList->AddRect(healthMin, healthMax, IM_COL32(255, 255, 255, 85), 4.0f);

    char healthText[64]{};
    if (runtime.combat.localPlayerAlive) {
        std::snprintf(healthText, sizeof(healthText), "HP %d", static_cast<int>(std::round(health)));
    } else {
        std::snprintf(healthText, sizeof(healthText), "HP 0");
    }
    drawList->AddText(ImVec2(healthBarX + 8.0f, healthBarY - 20.0f), IM_COL32(245, 245, 245, 255),
                      healthText);

    constexpr int hotbarCount = kHotbarSlots;
    const float slotWidth = 110.0f;
    const float slotHeight = 58.0f;
    const float slotSpacing = 8.0f;
    const float totalHotbarWidth = (slotWidth * hotbarCount) + (slotSpacing * (hotbarCount - 1));
    const float hotbarX = (io.DisplaySize.x - totalHotbarWidth) * 0.5f;
    const float hotbarY = io.DisplaySize.y - slotHeight - 18.0f;

    const bool hasInventorySnapshot = runtime.inventoryUi && runtime.inventoryUi->hasSnapshot();
    const std::array<Slot, kInventorySlotCount> *slots =
        hasInventorySnapshot ? &runtime.inventoryUi->slots() : nullptr;

    auto hotbarItemName = [](const Slot &slot) -> std::string {
        if (Inventory::IsEmpty(slot) || !Inventory::IsValidItemId(slot.itemId)) {
            return "Empty";
        }
        std::string name = Items::ItemDatabase[slot.itemId].name;
        if (name.empty()) {
            name = "Item " + std::to_string(slot.itemId);
        }
        if (name.size() > 12) {
            name.resize(12);
            name += ".";
        }
        return name;
    };

    for (int i = 0; i < hotbarCount; ++i) {
        const float x = hotbarX + i * (slotWidth + slotSpacing);
        const ImVec2 slotMin(x, hotbarY);
        const ImVec2 slotMax(x + slotWidth, hotbarY + slotHeight);

        Slot slot{};
        slot.itemId = kInventoryEmptyItemId;
        slot.quantity = 0;
        if (slots != nullptr) {
            slot = (*slots)[static_cast<size_t>(i)];
        }
        const bool empty = Inventory::IsEmpty(slot);
        const bool active = (static_cast<uint16_t>(i) == runtime.combat.activeHotbarSlot);

        drawList->AddRectFilled(slotMin, slotMax, IM_COL32(8, 8, 8, 170), 6.0f);
        drawList->AddRect(slotMin, slotMax,
                          active ? IM_COL32(245, 210, 120, 255) : IM_COL32(255, 255, 255, 75), 6.0f,
                          0, active ? 2.5f : 1.0f);

        const std::string indexText = std::to_string(i + 1);
        drawList->AddText(ImVec2(x + 6.0f, hotbarY + 4.0f), IM_COL32(210, 210, 210, 220),
                          indexText.c_str());

        const std::string name = hotbarItemName(slot);
        const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
        drawList->AddText(ImVec2(x + (slotWidth - nameSize.x) * 0.5f, hotbarY + 20.0f),
                          empty ? IM_COL32(140, 140, 140, 190) : IM_COL32(240, 240, 240, 255),
                          name.c_str());

        if (!empty) {
            const std::string qtyText = "x" + std::to_string(slot.quantity);
            const ImVec2 qtySize = ImGui::CalcTextSize(qtyText.c_str());
            drawList->AddText(
                ImVec2(x + slotWidth - qtySize.x - 6.0f, hotbarY + slotHeight - qtySize.y - 5.0f),
                IM_COL32(235, 235, 235, 255), qtyText.c_str());
        }
    }
}

void ClientHudSystem::drawDeathOverlay(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr || runtime.combat.localPlayerAlive) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    const ImVec2 displaySize = io.DisplaySize;
    const ImVec2 center(displaySize.x * 0.5f, displaySize.y * 0.5f);

    drawList->AddRectFilled(ImVec2(0.0f, 0.0f), displaySize, IM_COL32(0, 0, 0, 120));

    std::string title = "You were killed";
    if (!runtime.combat.localDeathKiller.empty()) {
        title += " by [";
        title += runtime.combat.localDeathKiller;
        title += "]";
    }
    char timerLine[64]{};
    const float secondsRemaining = std::max(0.0f, runtime.combat.localRespawnSeconds);
    if (secondsRemaining > 0.05f) {
        std::snprintf(timerLine, sizeof(timerLine), "Respawning in %.1fs", secondsRemaining);
    } else {
        std::snprintf(timerLine, sizeof(timerLine), "Click to respawn");
    }

    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    const ImVec2 timerSize = ImGui::CalcTextSize(timerLine);
    const float blockWidth = std::max(titleSize.x, timerSize.x);

    const ImVec2 bgMin(center.x - blockWidth * 0.5f - 24.0f, center.y - 42.0f);
    const ImVec2 bgMax(center.x + blockWidth * 0.5f + 24.0f, center.y + 34.0f);
    drawList->AddRectFilled(bgMin, bgMax, IM_COL32(12, 12, 12, 210), 8.0f);

    drawList->AddText(ImVec2(center.x - titleSize.x * 0.5f, center.y - 24.0f),
                      IM_COL32(255, 210, 210, 255), title.c_str());
    drawList->AddText(ImVec2(center.x - timerSize.x * 0.5f, center.y + 2.0f),
                      IM_COL32(235, 235, 235, 255), timerLine);
}
