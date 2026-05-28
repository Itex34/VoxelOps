#pragma once

#include "../../network/ClientNetwork.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
    class Event;
    class EventListener;
}

struct Runtime;

class InventoryUI {
public:
    InventoryUI();
    ~InventoryUI();

    void setVisible(bool visible) noexcept;
    void toggleVisible() noexcept;
    [[nodiscard]] bool isVisible() const noexcept;

    void reset();
    void consumeNetwork(ClientNetwork &clientNet);
    void draw(Runtime &runtime, ClientNetwork &clientNet, bool connected);
    [[nodiscard]] bool hasSnapshot() const noexcept;
    [[nodiscard]] const std::array<Slot, kInventorySlotCount> &slots() const noexcept;
    [[nodiscard]] uint32_t revision() const noexcept;

private:
    [[nodiscard]] bool hasAuthoritativeSnapshot() const noexcept;
    bool submitAction(
        ClientNetwork &clientNet,
        InventoryActionType type,
        uint16_t sourceSlot,
        uint16_t destinationSlot,
        uint16_t amount
    );
    void drawImGui(Runtime &runtime, ClientNetwork &clientNet, bool connected);
    void drawRml(Runtime &runtime, ClientNetwork &clientNet, bool connected);
    bool ensureRmlDocument(Runtime &runtime);
    void resetRmlDocument();
    void forgetRmlState();
    void syncRmlState(Runtime &runtime, bool connected);
    void handleRmlEvent(Runtime &runtime, ClientNetwork &clientNet, Rml::Event &event);

    bool m_visible = false;
    bool m_hasSnapshot = false;
    uint32_t m_revision = 0;
    std::array<Slot, kInventorySlotCount> m_slots{};
    int m_selectedSlot = -1;
    uint32_t m_nextRequestId = 1;
    std::string m_selectedItemName;

    class RmlInventoryListener;
    Rml::Context *m_rmlContext = nullptr;
    Rml::ElementDocument *m_rmlDocument = nullptr;
    Rml::Element *m_backdrop = nullptr;
    Rml::Element *m_panel = nullptr;
    Rml::Element *m_statusText = nullptr;
    Rml::Element *m_selectedText = nullptr;
    Rml::Element *m_useButton = nullptr;
    Rml::Element *m_dropOneButton = nullptr;
    Rml::Element *m_dropStackButton = nullptr;
    std::array<Rml::Element *, kInventorySlotCount> m_slotButtons{};
    std::unique_ptr<RmlInventoryListener> m_rmlListener;
    bool m_rmlDocumentVisible = false;
};
