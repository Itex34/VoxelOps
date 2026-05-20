#pragma once

#include "../network/ClientNetwork.hpp"
#include "../ui/debug/DebugUi.hpp"
#include "../ui/player/InventoryUI.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

enum class UiView : uint8_t {
    MainMenu = 0,
    InGame = 1
};

struct RuntimeUiState {
    struct KillFeedEntry {
        std::string killer;
        std::string victim;
        uint16_t weaponId = 0;
        double expiresAt = 0.0;
    };

    static constexpr size_t MaxKillFeedEntries = 8;
    static constexpr double KillFeedDurationSec = 5.0;

    std::unique_ptr<DebugUi> debugUi;
    std::unique_ptr<InventoryUI> inventoryUi;

    std::deque<KillFeedEntry> killFeedEntries;
    int matchRemainingSeconds = 600;
    bool matchStarted = false;
    bool matchEnded = false;
    std::string matchWinner;
    std::vector<ClientNetwork::ScoreboardEntry> scoreboardEntries;
    UiView activeView = UiView::MainMenu;
    bool wantsCursor = true;
};
