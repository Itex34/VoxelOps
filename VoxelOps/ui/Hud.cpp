#include "Hud.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"
#include "player/ItemIconUi.hpp"
#include "widgets/UIContext.hpp"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <format>

using namespace AppHelpers;

namespace {
    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }
} // namespace

void Hud::draw(Runtime &runtime) {
    if (runtime.ui.activeView != UiView::InGame) {
        return;
    }

    if (runtime.ui.nativeUi && runtime.ui.nativeUi->hasBackendRenderer()) {
        drawNative(runtime);
        return;
    }

    drawImGui(runtime);
}

void Hud::drawNative(Runtime &runtime) {
    if (!runtime.ui.nativeUi) {
        return;
    }

    UIContext &ui = runtime.ui.nativeUi->context();
    const glm::vec2 screen = ui.screenSize();

    const int pingMs = runtime.network.clientNet.GetPingMs();
    const std::string pingLine =
        (pingMs >= 0) ? ("Ping: " + std::to_string(pingMs) + " ms") : "Ping: --";
    Color pingColor{0.86f, 0.86f, 0.86f, 1.0f};
    if (pingMs >= 150) {
        pingColor = Color{1.0f, 0.48f, 0.0f, 1.0f};
    } else if (pingMs >= 80) {
        pingColor = Color{1.0f, 0.72f, 0.40f, 1.0f};
    }
    ui.panel(Rect{16.0f, 16.0f, 136.0f, 28.0f}, Color{0.0f, 0.0f, 0.0f, 0.48f});
    ui.label(pingLine, glm::vec2(24.0f, 22.0f), pingColor);

    {
        const double now = GetTimeSeconds();
        while (!runtime.ui.killFeedEntries.empty() && runtime.ui.killFeedEntries.back().expiresAt <= now) {
            runtime.ui.killFeedEntries.pop_back();
        }
        const std::string localName = runtime.network.clientNet.GetAssignedUsername();
        float y = 18.0f;
        const float width = 390.0f;
        const float x = std::max(16.0f, screen.x - width - 18.0f);
        for (const RuntimeUiState::KillFeedEntry &entry : runtime.ui.killFeedEntries) {
            std::string line = entry.killer + " [" +
                               std::string(GunTypeName(static_cast<GunType>(entry.weaponId))) +
                               "] " + entry.victim;
            if (line.size() > 48) {
                line.resize(47);
                line += ".";
            }
            Color color{0.86f, 0.86f, 0.86f, 1.0f};
            if (!localName.empty() && entry.killer == localName) {
                color = Color{1.0f, 0.72f, 0.40f, 1.0f};
            } else if (!localName.empty() && entry.victim == localName) {
                color = Color{1.0f, 0.48f, 0.0f, 1.0f};
            }
            ui.panel(Rect{x, y, width, 28.0f}, Color{0.02f, 0.02f, 0.02f, 0.62f});
            ui.label(line, glm::vec2(x + 8.0f, y + 6.0f), color);
            y += 34.0f;
        }
    }

    const bool showScoreboard = IsScancodeDown(SDL_SCANCODE_TAB) || runtime.ui.matchEnded;
    if (showScoreboard) {
        const float panelWidth = 560.0f;
        const float rowHeight = 24.0f;
        const float x = (screen.x - panelWidth) * 0.5f;
        const float y = 72.0f;
        const float panelHeight = 92.0f + rowHeight * static_cast<float>(runtime.ui.scoreboardEntries.size());
        ui.panel(Rect{x, y, panelWidth, panelHeight}, Color{0.04f, 0.04f, 0.04f, 0.86f});

        const int clampedRemaining = std::max(0, runtime.ui.matchRemainingSeconds);
        const int minutes = clampedRemaining / 60;
        const int seconds = clampedRemaining % 60;

        const std::string timerLine = runtime.ui.matchStarted
                                          ? std::format("Time Left: {:02}:{:02}", minutes, seconds)
                                          : "Waiting for players";

        std::string title = runtime.ui.matchEnded ? "Match Ended" : "Deathmatch";
        if (runtime.ui.matchEnded && !runtime.ui.matchWinner.empty()) {
            title += " - Winner: " + runtime.ui.matchWinner;
        }
        ui.label(title, glm::vec2(x + 22.0f, y + 16.0f), Color{0.96f, 0.96f, 0.96f, 1.0f});
        ui.label(timerLine, glm::vec2(x + 22.0f, y + 40.0f), Color{0.82f, 0.82f, 0.82f, 1.0f});
        ui.panel(Rect{x + 12.0f, y + 66.0f, panelWidth - 24.0f, 24.0f}, Color{0.14f, 0.14f, 0.14f, 0.78f});
        ui.label("Player", glm::vec2(x + 24.0f, y + 71.0f));
        ui.label("K", glm::vec2(x + 360.0f, y + 71.0f));
        ui.label("D", glm::vec2(x + 430.0f, y + 71.0f));
        ui.label("Ping", glm::vec2(x + 494.0f, y + 71.0f));

        const std::string localName = runtime.network.clientNet.GetAssignedUsername();
        float rowY = y + 94.0f;
        for (const auto &entry : runtime.ui.scoreboardEntries) {
            const Color nameColor = (!localName.empty() && entry.username == localName)
                                        ? Color{1.0f, 0.72f, 0.40f, 1.0f}
                                        : Color{0.90f, 0.90f, 0.90f, 1.0f};
            std::string name = entry.username;
            if (name.size() > 24) {
                name.resize(23);
                name += ".";
            }
            ui.label(name, glm::vec2(x + 24.0f, rowY), nameColor);
            ui.label(std::to_string(entry.kills), glm::vec2(x + 360.0f, rowY));
            ui.label(std::to_string(entry.deaths), glm::vec2(x + 430.0f, rowY));
            ui.label((entry.pingMs >= 0) ? std::to_string(entry.pingMs) : "--", glm::vec2(x + 494.0f, rowY));
            rowY += rowHeight;
        }
    }

    if (runtime.network.clientNet.IsConnected()) {
        const float health = std::clamp(runtime.combat.localHealth, 0.0f, 100.0f);
        const float healthPct = health / 100.0f;
        const float healthBarX = 24.0f;
        const float healthBarY = screen.y - 102.0f;
        ui.label("HP " + std::to_string(static_cast<int>(std::round(health))), glm::vec2(healthBarX, healthBarY - 22.0f));
        ui.panel(Rect{healthBarX, healthBarY, 240.0f, 18.0f}, Color{0.0f, 0.0f, 0.0f, 0.55f});
        ui.panel(
            Rect{healthBarX, healthBarY, 240.0f * healthPct, 18.0f},
            runtime.combat.localPlayerAlive ? Color{1.0f, 0.62f, 0.26f, 1.0f} : Color{1.0f, 0.30f, 0.0f, 1.0f}
        );

        constexpr int hotbarCount = kHotbarSlots;
        const float slotWidth = 110.0f;
        const float slotHeight = 58.0f;
        const float slotSpacing = 8.0f;
        const float totalHotbarWidth = (slotWidth * hotbarCount) + (slotSpacing * (hotbarCount - 1));
        const float hotbarX = (screen.x - totalHotbarWidth) * 0.5f;
        const float hotbarY = screen.y - slotHeight - 18.0f;

        const bool hasInventorySnapshot = runtime.ui.inventoryUi && runtime.ui.inventoryUi->hasSnapshot();
        const std::array<Slot, kInventorySlotCount> *slots =
            hasInventorySnapshot ? &runtime.ui.inventoryUi->slots() : nullptr;

        for (int i = 0; i < hotbarCount; ++i) {
            const float x = hotbarX + i * (slotWidth + slotSpacing);
            Slot slot{};
            slot.itemId = kInventoryEmptyItemId;
            slot.quantity = 0;
            if (slots != nullptr) {
                slot = (*slots)[static_cast<size_t>(i)];
            }

            const bool empty = Inventory::IsEmpty(slot);
            const bool active = (static_cast<uint16_t>(i) == runtime.combat.activeHotbarSlot);
            ui.panel(Rect{x, hotbarY, slotWidth, slotHeight}, Color{0.03f, 0.03f, 0.03f, 0.68f});
            if (active) {
                ui.panel(Rect{x, hotbarY, slotWidth, 3.0f}, Color{1.0f, 0.72f, 0.40f, 1.0f});
            }

            ui.labelInRect(
                std::to_string(i + 1),
                Rect{x + 6.0f, hotbarY + 4.0f, 18.0f, 18.0f},
                Color{0.82f, 0.82f, 0.82f, 0.9f},
                TextAlign::Center,
                TextVerticalAlign::Center
            );
            const TextureHandle blockTexture =
                runtime.ui.nativeUi ? ItemIconUi::blockTextureForSlot(*runtime.ui.nativeUi, slot) : 0;
            std::string name = "Empty";
            if (!empty && Inventory::IsValidItemId(slot.itemId)) {
                name = Items::ItemDatabase[slot.itemId].name;
                if (name.empty()) {
                    name = "Item " + std::to_string(slot.itemId);
                }
                if (name.size() > 12) {
                    name.resize(11);
                    name += ".";
                }
            }
            if (blockTexture != 0) {
                ui.image(blockTexture, Rect{x + 36.0f, hotbarY + 13.0f, 38.0f, 38.0f});
            } else {
                ui.labelInRect(
                    name,
                    Rect{x + 10.0f, hotbarY + 18.0f, slotWidth - 20.0f, 18.0f},
                    empty ? Color{0.55f, 0.55f, 0.55f, 0.86f} : Color{0.94f, 0.94f, 0.94f, 1.0f},
                    TextAlign::Center,
                    TextVerticalAlign::Center
                );
            }
            if (!empty) {
                ui.labelInRect(
                    "x" + std::to_string(slot.quantity),
                    Rect{x + slotWidth - 48.0f, hotbarY + 38.0f, 40.0f, 16.0f},
                    Color{1.0f, 1.0f, 1.0f, 1.0f},
                    TextAlign::End,
                    TextVerticalAlign::Center
                );
            }
        }
    }

    if (!GameData::cursorEnabled && runtime.combat.localPlayerAlive) {
        const float cx = screen.x * 0.5f;
        const float cy = screen.y * 0.5f;
        ui.panel(Rect{cx - 7.0f, cy - 1.0f, 14.0f, 2.0f}, Color{1.0f, 1.0f, 1.0f, 0.85f});
        ui.panel(Rect{cx - 1.0f, cy - 7.0f, 2.0f, 14.0f}, Color{1.0f, 1.0f, 1.0f, 0.85f});
    }

    if (!runtime.combat.localPlayerAlive) {
        ui.panel(Rect{0.0f, 0.0f, screen.x, screen.y}, Color{0.0f, 0.0f, 0.0f, 0.45f});
        const float panelWidth = 520.0f;
        const float x = (screen.x - panelWidth) * 0.5f;
        const float y = (screen.y * 0.5f) - 42.0f;
        ui.panel(Rect{x, y, panelWidth, 86.0f}, Color{0.05f, 0.05f, 0.05f, 0.84f});

        std::string title = "You were killed";
        if (!runtime.combat.localDeathKiller.empty()) {
            title += " by [" + runtime.combat.localDeathKiller + "]";
        }
        ui.label(title, glm::vec2(x + 24.0f, y + 18.0f), Color{1.0f, 0.82f, 0.82f, 1.0f});

        const float secondsRemaining = std::max(0.0f, runtime.combat.localRespawnSeconds);
        const std::string respawnLine = secondsRemaining > 0.05f
                                            ? std::format("Respawning in {:.1f}s", secondsRemaining)
                                            : "Click to respawn";
        ui.label(respawnLine, glm::vec2(x + 24.0f, y + 48.0f));
    }
}

void Hud::drawImGui(Runtime &runtime) {

}
