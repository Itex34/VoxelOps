#pragma once

#include "../runtime/Runtime.hpp"
#include <array>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

class Hud {
public:
    Hud() = default;
    ~Hud() = default;
    void draw(Runtime &runtime);

private:
    void drawImGui(Runtime &runtime);
    void drawKillFeedImGui(Runtime &runtime);
    void drawScoreboardImGui(Runtime &runtime);
    void drawPingCounterImGui(Runtime &runtime);
    void drawPlayerHudImGui(Runtime &runtime);
    void drawDeathOverlayImGui(Runtime &runtime);

    void drawRml(Runtime &runtime);
    bool ensureRmlDocument(Runtime &runtime);
    void resetRmlDocument();
    void syncRmlState(Runtime &runtime);

    Rml::Context *m_rmlContext = nullptr;
    Rml::ElementDocument *m_rmlDocument = nullptr;
    Rml::Element *m_pingText = nullptr;
    Rml::Element *m_killFeedText = nullptr;
    Rml::Element *m_scoreboardPanel = nullptr;
    Rml::Element *m_scoreboardTitle = nullptr;
    Rml::Element *m_scoreboardTimer = nullptr;
    Rml::Element *m_scoreboardRows = nullptr;
    Rml::Element *m_healthFill = nullptr;
    Rml::Element *m_healthText = nullptr;
    std::array<Rml::Element *, kHotbarSlots> m_hotbarSlots{};
    Rml::Element *m_deathOverlay = nullptr;
    Rml::Element *m_deathTitle = nullptr;
    Rml::Element *m_deathTimer = nullptr;
    Rml::Element *m_crosshair = nullptr;
    bool m_rmlDocumentVisible = false;
};
