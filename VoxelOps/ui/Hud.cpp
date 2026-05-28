#include "Hud.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

using namespace AppHelpers;

namespace {
    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }

    std::string EscapeRml(std::string_view text) {
        std::string escaped;
        escaped.reserve(text.size() + 16);
        for (const char c : text) {
            switch (c) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            default:
                escaped.push_back(c);
                break;
            }
        }
        return escaped;
    }
} // namespace

void Hud::draw(Runtime &runtime) {
    if (runtime.ui.activeView != UiView::InGame) {
        if (m_rmlDocument && m_rmlDocumentVisible) {
            m_rmlDocument->Hide();
            m_rmlDocumentVisible = false;
        }
        return;
    }

    if (runtime.ui.rmlUi && runtime.ui.rmlUi->isUsingOpenGlBackend()) {
        drawRml(runtime);
        return;
    }
    drawImGui(runtime);
}

bool Hud::ensureRmlDocument(Runtime &runtime) {
    if (!runtime.ui.rmlUi || !runtime.ui.rmlUi->isUsingOpenGlBackend()) {
        resetRmlDocument();
        return false;
    }

    Rml::Context *context = runtime.ui.rmlUi->context();
    if (context == nullptr) {
        resetRmlDocument();
        return false;
    }

    if (m_rmlContext == context && m_rmlDocument != nullptr) {
        return true;
    }

    resetRmlDocument();
    m_rmlContext = context;

    const std::string hudPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("ui/rml/documents/hud.rml").generic_string();
    m_rmlDocument = m_rmlContext->LoadDocument(hudPath);
    if (!m_rmlDocument) {
        std::cerr << "[HUD][RmlUi] Failed to load document: " << hudPath << "\n";
        return false;
    }

    auto requireElement = [this](const char *id) -> Rml::Element * {
        Rml::Element *element = m_rmlDocument->GetElementById(id);
        if (!element) {
            std::cerr << "[HUD][RmlUi] Missing required element id='" << id
                      << "' in ui/rml/documents/hud.rml\n";
        }
        return element;
    };

    m_pingText = requireElement("ping");
    m_killFeedText = requireElement("killfeed");
    m_scoreboardPanel = requireElement("scoreboard");
    m_scoreboardTitle = requireElement("score_title");
    m_scoreboardTimer = requireElement("score_timer");
    m_scoreboardRows = requireElement("score_rows");
    m_healthFill = requireElement("health_fill");
    m_healthText = requireElement("health_label");
    m_hotbarSlots.fill(nullptr);
    for (int i = 0; i < kHotbarSlots; ++i) {
        m_hotbarSlots[static_cast<size_t>(i)] =
            requireElement(("hotbar_slot_" + std::to_string(i)).c_str());
    }
    m_deathOverlay = requireElement("death_overlay");
    m_deathTitle = requireElement("death_title");
    m_deathTimer = requireElement("death_timer");
    m_crosshair = requireElement("crosshair");

    if (!m_pingText || !m_killFeedText || !m_scoreboardPanel || !m_scoreboardTitle ||
        !m_scoreboardTimer || !m_scoreboardRows || !m_healthFill || !m_healthText ||
        !m_deathOverlay || !m_deathTitle || !m_deathTimer || !m_crosshair) {
        resetRmlDocument();
        std::cerr << "[HUD][RmlUi] HUD document is missing one or more required nodes; falling back to ImGui.\n";
        return false;
    }
    for (Rml::Element *slot : m_hotbarSlots) {
        if (!slot) {
            resetRmlDocument();
            std::cerr << "[HUD][RmlUi] HUD hotbar nodes are incomplete; falling back to ImGui.\n";
            return false;
        }
    }

    m_rmlDocument->Show();
    m_rmlDocumentVisible = true;
    return true;
}

void Hud::resetRmlDocument() {
    if (m_rmlDocument) {
        m_rmlDocument->Close();
    }
    m_pingText = nullptr;
    m_killFeedText = nullptr;
    m_scoreboardPanel = nullptr;
    m_scoreboardTitle = nullptr;
    m_scoreboardTimer = nullptr;
    m_scoreboardRows = nullptr;
    m_healthFill = nullptr;
    m_healthText = nullptr;
    m_hotbarSlots.fill(nullptr);
    m_deathOverlay = nullptr;
    m_deathTitle = nullptr;
    m_deathTimer = nullptr;
    m_crosshair = nullptr;
    m_rmlDocument = nullptr;
    m_rmlContext = nullptr;
    m_rmlDocumentVisible = false;
}

void Hud::syncRmlState(Runtime &runtime) {
    if (!m_rmlDocument) {
        return;
    }

    const int pingMs = runtime.network.clientNet.GetPingMs();
    if (m_pingText) {
        m_pingText->SetInnerRML(
            (pingMs >= 0) ? ("Ping: " + std::to_string(pingMs) + " ms") : std::string("Ping: --")
        );
        if (pingMs >= 150) {
            m_pingText->SetProperty("color", "#ff7b00");
        } else if (pingMs >= 80) {
            m_pingText->SetProperty("color", "#ffb866");
        } else {
            m_pingText->SetProperty("color", "#d7d7d7");
        }
    }

    {
        const double now = GetTimeSeconds();
        while (!runtime.ui.killFeedEntries.empty() && runtime.ui.killFeedEntries.back().expiresAt <= now) {
            runtime.ui.killFeedEntries.pop_back();
        }
        const std::string localName = runtime.network.clientNet.GetAssignedUsername();
        std::string killFeedRml;
        for (const RuntimeUiState::KillFeedEntry &entry : runtime.ui.killFeedEntries) {
            std::string line = EscapeRml(entry.killer) + " [" +
                               EscapeRml(std::string(GunTypeName(static_cast<GunType>(entry.weaponId)))) +
                               "] " + EscapeRml(entry.victim);
            std::string color = "#d7d7d7";
            if (!localName.empty() && entry.killer == localName) {
                color = "#ffb866";
            } else if (!localName.empty() && entry.victim == localName) {
                color = "#ff7b00";
            }
            killFeedRml += "<div style='margin-bottom:6px; background-color:#1c1c1c; border:1px #4a4a4a; padding:3px 8px; border-radius:4px; color:";
            killFeedRml += color;
            killFeedRml += ";'>";
            killFeedRml += line;
            killFeedRml += "</div>";
        }
        if (m_killFeedText) {
            m_killFeedText->SetInnerRML(killFeedRml);
        }
    }

    const bool showScoreboard = IsScancodeDown(SDL_SCANCODE_TAB) || runtime.ui.matchEnded;
    if (m_scoreboardPanel) {
        m_scoreboardPanel->SetProperty("display", showScoreboard ? "block" : "none");
    }
    if (showScoreboard) {
        const int clampedRemaining = std::max(0, runtime.ui.matchRemainingSeconds);
        const int minutes = clampedRemaining / 60;
        const int seconds = clampedRemaining % 60;
        char timerLine[64]{};
        if (!runtime.ui.matchStarted) {
            std::snprintf(timerLine, sizeof(timerLine), "Waiting for players");
        } else {
            std::snprintf(timerLine, sizeof(timerLine), "Time Left: %02d:%02d", minutes, seconds);
        }

        std::string title = "Deathmatch";
        if (runtime.ui.matchEnded) {
            title = "Match Ended";
            if (!runtime.ui.matchWinner.empty()) {
                title += " - Winner: ";
                title += runtime.ui.matchWinner;
            }
        }

        if (m_scoreboardTitle) {
            m_scoreboardTitle->SetInnerRML(EscapeRml(title));
        }
        if (m_scoreboardTimer) {
            m_scoreboardTimer->SetInnerRML(EscapeRml(timerLine));
        }
        if (m_scoreboardRows) {
            std::string rows = "Player                      K    D    Ping<br/>";
            const std::string localName = runtime.network.clientNet.GetAssignedUsername();
            for (const auto &entry : runtime.ui.scoreboardEntries) {
                std::string name = entry.username;
                if (name.size() > 24) {
                    name.resize(24);
                }
                rows += (entry.username == localName) ? "<span style='color:#ffb866'>" : "";
                rows += EscapeRml(name);
                if (entry.username == localName) {
                    rows += "</span>";
                }
                if (name.size() < 24) {
                    rows.append(24 - name.size(), ' ');
                }
                rows += std::to_string(entry.kills);
                rows += "    ";
                rows += std::to_string(entry.deaths);
                rows += "    ";
                rows += (entry.pingMs >= 0) ? std::to_string(entry.pingMs) : "--";
                rows += "<br/>";
            }
            m_scoreboardRows->SetInnerRML(rows);
        }
    }

    const float health = std::clamp(runtime.combat.localHealth, 0.0f, 100.0f);
    const float healthPct = health / 100.0f;
    if (m_healthFill) {
        m_healthFill->SetProperty("width", std::to_string(healthPct * 100.0f) + "%");
        m_healthFill->SetProperty("background-color", runtime.combat.localPlayerAlive ? "#ff9f43" : "#ff7b00");
    }
    if (m_healthText) {
        char healthText[64]{};
        if (runtime.combat.localPlayerAlive) {
            std::snprintf(healthText, sizeof(healthText), "HP %d", static_cast<int>(std::round(health)));
        } else {
            std::snprintf(healthText, sizeof(healthText), "HP 0");
        }
        m_healthText->SetInnerRML(healthText);
    }

    {
        const bool hasInventorySnapshot = runtime.ui.inventoryUi && runtime.ui.inventoryUi->hasSnapshot();
        const std::array<Slot, kInventorySlotCount> *slots =
            hasInventorySnapshot ? &runtime.ui.inventoryUi->slots() : nullptr;

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
            }
            return name;
        };

        for (int i = 0; i < kHotbarSlots; ++i) {
            Rml::Element *slotElement = m_hotbarSlots[static_cast<size_t>(i)];
            if (!slotElement) {
                continue;
            }
            Slot slot{};
            slot.itemId = kInventoryEmptyItemId;
            slot.quantity = 0;
            if (slots != nullptr) {
                slot = (*slots)[static_cast<size_t>(i)];
            }

            const std::string name = EscapeRml(hotbarItemName(slot));
            const std::string qty = Inventory::IsEmpty(slot) ? "" : ("x" + std::to_string(slot.quantity));
            std::ostringstream inner;
            inner << "<div class='hotbar_index'>" << (i + 1) << "</div>"
                  << "<div class='hotbar_name'>" << name << "</div>"
                  << "<div class='hotbar_qty'>" << EscapeRml(qty) << "</div>";
            slotElement->SetInnerRML(inner.str());

            const bool isActive = (static_cast<uint16_t>(i) == runtime.combat.activeHotbarSlot);
            slotElement->SetClass("active", isActive);
        }
    }

    if (m_deathOverlay) {
        m_deathOverlay->SetProperty("display", runtime.combat.localPlayerAlive ? "none" : "block");
    }
    if (!runtime.combat.localPlayerAlive) {
        if (m_deathTitle) {
            std::string title = "You were killed";
            if (!runtime.combat.localDeathKiller.empty()) {
                title += " by [";
                title += runtime.combat.localDeathKiller;
                title += "]";
            }
            m_deathTitle->SetInnerRML(EscapeRml(title));
        }
        if (m_deathTimer) {
            char timerLine[64]{};
            const float secondsRemaining = std::max(0.0f, runtime.combat.localRespawnSeconds);
            if (secondsRemaining > 0.05f) {
                std::snprintf(timerLine, sizeof(timerLine), "Respawning in %.1fs", secondsRemaining);
            } else {
                std::snprintf(timerLine, sizeof(timerLine), "Click to respawn");
            }
            m_deathTimer->SetInnerRML(timerLine);
        }
    }

    if (m_crosshair) {
        const bool showCrosshair = !GameData::cursorEnabled && runtime.combat.localPlayerAlive;
        m_crosshair->SetProperty("display", showCrosshair ? "block" : "none");
    }
}

void Hud::drawRml(Runtime &runtime) {
    if (!ensureRmlDocument(runtime)) {
        drawImGui(runtime);
        return;
    }
    if (m_rmlDocument && !m_rmlDocumentVisible) {
        m_rmlDocument->Show();
        m_rmlDocumentVisible = true;
    }
    syncRmlState(runtime);
}

void Hud::drawImGui(Runtime &runtime) {
    drawScoreboardImGui(runtime);
    drawPingCounterImGui(runtime);
    drawKillFeedImGui(runtime);
    drawPlayerHudImGui(runtime);
    drawDeathOverlayImGui(runtime);
}

void Hud::drawKillFeedImGui(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr || runtime.ui.killFeedEntries.empty()) {
        return;
    }

    const double now = GetTimeSeconds();
    while (!runtime.ui.killFeedEntries.empty() && runtime.ui.killFeedEntries.back().expiresAt <= now) {
        runtime.ui.killFeedEntries.pop_back();
    }
    if (runtime.ui.killFeedEntries.empty()) {
        return;
    }

    const std::string localName = runtime.network.clientNet.GetAssignedUsername();
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *drawList = ImGui::GetForegroundDrawList();
    float y = 24.0f;

    for (const RuntimeUiState::KillFeedEntry &entry : runtime.ui.killFeedEntries) {
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

void Hud::drawScoreboardImGui(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const bool showScoreboard = IsScancodeDown(SDL_SCANCODE_TAB) || runtime.ui.matchEnded;
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
                              rowHeight * static_cast<float>(runtime.ui.scoreboardEntries.size()) +
                              14.0f;
    const float x = (io.DisplaySize.x - panelWidth) * 0.5f;
    const float y = 72.0f;

    drawList->AddRectFilled(
        ImVec2(x, y), ImVec2(x + panelWidth, y + panelHeight), IM_COL32(10, 10, 10, 215), 8.0f
    );

    const int clampedRemaining = std::max(0, runtime.ui.matchRemainingSeconds);
    const int minutes = clampedRemaining / 60;
    const int seconds = clampedRemaining % 60;
    char timerLine[64]{};
    if (!runtime.ui.matchStarted) {
        std::snprintf(timerLine, sizeof(timerLine), "Waiting for players");
    } else {
        std::snprintf(timerLine, sizeof(timerLine), "Time Left: %02d:%02d", minutes, seconds);
    }

    std::string title = "Deathmatch";
    if (runtime.ui.matchEnded) {
        title = "Match Ended";
        if (!runtime.ui.matchWinner.empty()) {
            title += " - Winner: ";
            title += runtime.ui.matchWinner;
        }
    }

    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    drawList->AddText(
        ImVec2(x + (panelWidth - titleSize.x) * 0.5f, y + 12.0f),
        IM_COL32(245, 245, 245, 255),
        title.c_str()
    );
    const ImVec2 timerSize = ImGui::CalcTextSize(timerLine);
    drawList->AddText(
        ImVec2(x + (panelWidth - timerSize.x) * 0.5f, y + 34.0f),
        IM_COL32(210, 210, 210, 255),
        timerLine
    );

    const float tableY = y + headerHeight;
    drawList->AddRectFilled(
        ImVec2(x + 8.0f, tableY),
        ImVec2(x + panelWidth - 8.0f, tableY + tableHeaderHeight),
        IM_COL32(32, 32, 32, 220),
        4.0f
    );

    const float nameX = x + 24.0f;
    const float killsX = x + 360.0f;
    const float deathsX = x + 430.0f;
    const float pingX = x + 495.0f;
    drawList->AddText(ImVec2(nameX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "Player");
    drawList->AddText(ImVec2(killsX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "K");
    drawList->AddText(ImVec2(deathsX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "D");
    drawList->AddText(ImVec2(pingX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "Ping");

    const std::string localName = runtime.network.clientNet.GetAssignedUsername();
    float rowY = tableY + tableHeaderHeight;
    for (size_t i = 0; i < runtime.ui.scoreboardEntries.size(); ++i) {
        const ClientNetwork::ScoreboardEntry &entry = runtime.ui.scoreboardEntries[i];
        const bool oddRow = ((i % 2) != 0);
        if (oddRow) {
            drawList->AddRectFilled(
                ImVec2(x + 8.0f, rowY),
                ImVec2(x + panelWidth - 8.0f, rowY + rowHeight),
                IM_COL32(20, 20, 20, 145),
                0.0f
            );
        }

        ImU32 nameColor = IM_COL32(230, 230, 230, 255);
        if (!localName.empty() && entry.username == localName) {
            nameColor = IM_COL32(130, 255, 160, 255);
        }
        drawList->AddText(ImVec2(nameX, rowY + 4.0f), nameColor, entry.username.c_str());
        drawList->AddText(
            ImVec2(killsX, rowY + 4.0f),
            IM_COL32(230, 230, 230, 255),
            std::to_string(entry.kills).c_str()
        );
        drawList->AddText(
            ImVec2(deathsX, rowY + 4.0f),
            IM_COL32(230, 230, 230, 255),
            std::to_string(entry.deaths).c_str()
        );
        const std::string pingText =
            (entry.pingMs >= 0) ? std::to_string(entry.pingMs) : std::string("--");
        drawList->AddText(
            ImVec2(pingX, rowY + 4.0f), IM_COL32(230, 230, 230, 255), pingText.c_str()
        );

        rowY += rowHeight;
    }
}

void Hud::drawPingCounterImGui(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const int pingMs = runtime.network.clientNet.GetPingMs();
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

void Hud::drawPlayerHudImGui(Runtime &runtime) {
    if (ImGui::GetCurrentContext() == nullptr || !runtime.network.clientNet.IsConnected()) {
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
    const ImU32 healthColor =
        runtime.combat.localPlayerAlive ? IM_COL32(120, 220, 120, 255) : IM_COL32(220, 80, 80, 255);
    drawList->AddRectFilled(
        healthMin,
        ImVec2(healthBarX + (healthBarWidth * healthPct), healthBarY + healthBarHeight),
        healthColor,
        4.0f
    );
    drawList->AddRect(healthMin, healthMax, IM_COL32(255, 255, 255, 85), 4.0f);

    char healthText[64]{};
    if (runtime.combat.localPlayerAlive) {
        std::snprintf(
            healthText, sizeof(healthText), "HP %d", static_cast<int>(std::round(health))
        );
    } else {
        std::snprintf(healthText, sizeof(healthText), "HP 0");
    }
    drawList->AddText(
        ImVec2(healthBarX + 8.0f, healthBarY - 20.0f), IM_COL32(245, 245, 245, 255), healthText
    );

    constexpr int hotbarCount = kHotbarSlots;
    const float slotWidth = 110.0f;
    const float slotHeight = 58.0f;
    const float slotSpacing = 8.0f;
    const float totalHotbarWidth = (slotWidth * hotbarCount) + (slotSpacing * (hotbarCount - 1));
    const float hotbarX = (io.DisplaySize.x - totalHotbarWidth) * 0.5f;
    const float hotbarY = io.DisplaySize.y - slotHeight - 18.0f;

    const bool hasInventorySnapshot = runtime.ui.inventoryUi && runtime.ui.inventoryUi->hasSnapshot();
    const std::array<Slot, kInventorySlotCount> *slots =
        hasInventorySnapshot ? &runtime.ui.inventoryUi->slots() : nullptr;

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
        drawList->AddRect(
            slotMin,
            slotMax,
            active ? IM_COL32(245, 210, 120, 255) : IM_COL32(255, 255, 255, 75),
            6.0f,
            0,
            active ? 2.5f : 1.0f
        );

        const std::string indexText = std::to_string(i + 1);
        drawList->AddText(
            ImVec2(x + 6.0f, hotbarY + 4.0f), IM_COL32(210, 210, 210, 220), indexText.c_str()
        );

        const std::string name = hotbarItemName(slot);
        const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
        drawList->AddText(
            ImVec2(x + (slotWidth - nameSize.x) * 0.5f, hotbarY + 20.0f),
            empty ? IM_COL32(140, 140, 140, 190) : IM_COL32(240, 240, 240, 255),
            name.c_str()
        );

        if (!empty) {
            const std::string qtyText = "x" + std::to_string(slot.quantity);
            const ImVec2 qtySize = ImGui::CalcTextSize(qtyText.c_str());
            drawList->AddText(
                ImVec2(x + slotWidth - qtySize.x - 6.0f, hotbarY + slotHeight - qtySize.y - 5.0f),
                IM_COL32(235, 235, 235, 255),
                qtyText.c_str()
            );
        }
    }
}

void Hud::drawDeathOverlayImGui(Runtime &runtime) {
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

    drawList->AddText(
        ImVec2(center.x - titleSize.x * 0.5f, center.y - 24.0f),
        IM_COL32(255, 210, 210, 255),
        title.c_str()
    );
    drawList->AddText(
        ImVec2(center.x - timerSize.x * 0.5f, center.y + 2.0f),
        IM_COL32(235, 235, 235, 255),
        timerLine
    );
}
