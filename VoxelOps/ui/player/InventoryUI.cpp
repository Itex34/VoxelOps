#include "InventoryUI.hpp"

#include "../../../Shared/items/Items.hpp"
#include "../../runtime/Runtime.hpp"
#include "ItemIconUi.hpp"
#include "../widgets/UIContext.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
    constexpr uint16_t kHotbarStart = 0;
    constexpr uint16_t kHotbarEnd = kHotbarStart + kHotbarSlots;
    constexpr uint16_t kBackpackStart = kHotbarEnd;
    constexpr uint16_t kBackpackEnd = kBackpackStart + kBackpackSlots;
    constexpr uint16_t kAmmoStart = kBackpackEnd;
    constexpr uint16_t kAmmoEnd = kAmmoStart + kAmmoSlots;

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

InventoryUI::InventoryUI() = default;
InventoryUI::~InventoryUI() = default;

void InventoryUI::setVisible(bool visible) noexcept {
    m_visible = visible;
    if (!m_visible) {
        clearSelection();
    }
}

void InventoryUI::toggleVisible() noexcept {
    setVisible(!m_visible);
}

bool InventoryUI::isVisible() const noexcept {
    return m_visible;
}

bool InventoryUI::hasSnapshot() const noexcept {
    return m_hasSnapshot;
}

const std::array<Slot, kInventorySlotCount> &InventoryUI::slots() const noexcept {
    return m_slots;
}

uint32_t InventoryUI::revision() const noexcept {
    return m_revision;
}

void InventoryUI::reset() {
    m_hasSnapshot = false;
    m_revision = 0;
    clearSelection();
    for (Slot &slot : m_slots) {
        slot.itemId = kInventoryEmptyItemId;
        slot.quantity = 0;
    }
}

bool InventoryUI::hasAuthoritativeSnapshot() const noexcept {
    return m_hasSnapshot;
}

bool InventoryUI::submitAction(
    ClientNetwork &clientNet,
    InventoryActionType type,
    uint16_t sourceSlot,
    uint16_t destinationSlot,
    uint16_t amount
) {
    if (!hasAuthoritativeSnapshot()) {
        return false;
    }

    InventoryActionRequest request{};
    request.requestId = m_nextRequestId++;
    request.expectedRevision = m_revision;
    request.action.type = type;
    request.action.sourceSlot = sourceSlot;
    request.action.destinationSlot = destinationSlot;
    request.action.amount = amount;

    return clientNet.SendInventoryActionRequest(request);
}

void InventoryUI::consumeNetwork(ClientNetwork &clientNet) {
    InventorySnapshot snapshot{};
    while (clientNet.PopInventorySnapshot(snapshot)) {
        if (snapshot.slots.size() != kInventorySlotCount) {
            continue;
        }
        m_hasSnapshot = true;
        m_revision = snapshot.revision;
        for (uint16_t i = 0; i < kInventorySlotCount; ++i) {
            m_slots[i] = snapshot.slots[i];
        }
        if (m_selectedSlot >= 0 && m_selectedSlot < static_cast<int>(kInventorySlotCount)) {
            const Slot &selected = m_slots[static_cast<size_t>(m_selectedSlot)];
            if (Inventory::IsEmpty(selected)) {
                clearSelection();
            }
        }
    }

    InventoryActionResult result{};
    while (clientNet.PopInventoryActionResult(result)) {
        (void)result;
    }
}

void InventoryUI::draw(Runtime &runtime, ClientNetwork &clientNet, bool connected) {
    if (!m_visible) {
        return;
    }

    if (runtime.ui.nativeUi && runtime.ui.nativeUi->hasBackendRenderer()) {
        drawNative(runtime, clientNet, connected);
        return;
    }
}

void InventoryUI::clearSelection() {
    m_selectedSlot = -1;
    m_selectedItemName.clear();
}

std::string InventoryUI::slotDisplayName(uint16_t slotIndex) const {
    if (slotIndex >= kInventorySlotCount) {
        return "Empty";
    }

    const Slot &slot = m_slots[slotIndex];
    if (Inventory::IsEmpty(slot)) {
        return "Empty";
    }
    if (Inventory::IsValidItemId(slot.itemId)) {
        std::string itemName = Items::ItemDatabase[slot.itemId].name;
        if (!itemName.empty()) {
            return itemName;
        }
    }
    return "Item " + std::to_string(slot.itemId);
}

std::string InventoryUI::slotTitle(uint16_t slotIndex) const {
    if (slotIndex >= kAmmoStart && slotIndex < kAmmoEnd) {
        return "Ammo " + std::to_string(slotIndex - kAmmoStart + 1);
    }
    if (slotIndex >= kBackpackStart && slotIndex < kBackpackEnd) {
        return "Bag " + std::to_string(slotIndex - kBackpackStart + 1);
    }
    return "Hotbar " + std::to_string(slotIndex - kHotbarStart + 1);
}

std::string InventoryUI::selectedLine() {
    if (m_selectedSlot < 0 || m_selectedSlot >= static_cast<int>(kInventorySlotCount)) {
        return "No slot selected.";
    }

    const uint16_t slotIndex = static_cast<uint16_t>(m_selectedSlot);
    const Slot &slot = m_slots[slotIndex];
    if (Inventory::IsEmpty(slot)) {
        clearSelection();
        return "No slot selected.";
    }

    if (m_selectedItemName.empty()) {
        m_selectedItemName = slotDisplayName(slotIndex);
    }
    return "Slot " + std::to_string(slotIndex) + ": " + m_selectedItemName + " x" +
           std::to_string(slot.quantity);
}

void InventoryUI::selectOrMoveSlot(ClientNetwork &clientNet, uint16_t slotIndex) {
    if (!m_hasSnapshot || slotIndex >= kInventorySlotCount) {
        return;
    }

    const Slot &slot = m_slots[slotIndex];
    const bool empty = Inventory::IsEmpty(slot);
    const std::string itemName = slotDisplayName(slotIndex);

    if (m_selectedSlot < 0) {
        if (!empty) {
            m_selectedSlot = static_cast<int>(slotIndex);
            m_selectedItemName = itemName;
        }
        return;
    }

    if (m_selectedSlot == static_cast<int>(slotIndex)) {
        clearSelection();
        return;
    }

    const uint16_t sourceSlot = static_cast<uint16_t>(m_selectedSlot);
    if (sourceSlot >= kInventorySlotCount) {
        clearSelection();
        return;
    }

    const Slot &sourceSlotData = m_slots[sourceSlot];
    if (Inventory::IsEmpty(sourceSlotData)) {
        clearSelection();
        return;
    }
    if (!Inventory::IsItemAllowedInSlot(sourceSlotData.itemId, slotIndex)) {
        return;
    }

    InventoryActionType actionType = InventoryActionType::Move;
    bool canSubmit = true;
    if (!empty && slot.itemId != sourceSlotData.itemId) {
        if (!Inventory::IsItemAllowedInSlot(slot.itemId, sourceSlot)) {
            canSubmit = false;
        } else {
            actionType = InventoryActionType::Swap;
        }
    }
    if (canSubmit && submitAction(clientNet, actionType, sourceSlot, slotIndex, 0)) {
        clearSelection();
    }
}

void InventoryUI::useSelected(ClientNetwork &clientNet) {
    if (m_selectedSlot < 0 || m_selectedSlot >= static_cast<int>(kInventorySlotCount)) {
        return;
    }
    const uint16_t slotIndex = static_cast<uint16_t>(m_selectedSlot);
    if (Inventory::IsEmpty(m_slots[slotIndex])) {
        clearSelection();
        return;
    }
    (void)submitAction(clientNet, InventoryActionType::Use, slotIndex, slotIndex, 1);
}

void InventoryUI::dropSelected(ClientNetwork &clientNet, uint16_t amount) {
    if (m_selectedSlot < 0 || m_selectedSlot >= static_cast<int>(kInventorySlotCount)) {
        return;
    }
    const uint16_t slotIndex = static_cast<uint16_t>(m_selectedSlot);
    if (Inventory::IsEmpty(m_slots[slotIndex])) {
        clearSelection();
        return;
    }
    (void)submitAction(clientNet, InventoryActionType::Drop, slotIndex, slotIndex, amount);
}

void InventoryUI::drawNative(Runtime &runtime, ClientNetwork &clientNet, bool connected) {
    UIContext &ui = runtime.ui.nativeUi->context();
    const glm::vec2 screen = ui.screenSize();
    ui.panel(Rect{0.0f, 0.0f, screen.x, screen.y}, Color{0.0f, 0.0f, 0.0f, 0.52f});

    const float panelWidth = std::min(860.0f, std::max(520.0f, screen.x - 48.0f));
    const float panelHeight = std::min(560.0f, std::max(500.0f, screen.y - 48.0f));
    const float panelX = (screen.x - panelWidth) * 0.5f;
    const float panelY = (screen.y - panelHeight) * 0.5f;

    ui.panel(Rect{panelX, panelY, panelWidth, panelHeight}, Color{0.035f, 0.038f, 0.043f, 0.94f});
    ui.panel(Rect{panelX, panelY, panelWidth, 2.0f}, Color{1.0f, 0.63f, 0.22f, 1.0f});
    ui.label("Inventory", glm::vec2(panelX + 24.0f, panelY + 20.0f), Color{0.98f, 0.98f, 0.98f, 1.0f});

    const TextureHandle closeIcon = runtime.ui.nativeUi->icon(NativeUiIcon::Close);
    const TextureHandle useIcon = runtime.ui.nativeUi->icon(NativeUiIcon::Use);
    const TextureHandle dropOneIcon = runtime.ui.nativeUi->icon(NativeUiIcon::DropOne);
    const TextureHandle dropStackIcon = runtime.ui.nativeUi->icon(NativeUiIcon::DropStack);

    if (ui.iconButton("inventory_close", closeIcon, Rect{panelX + panelWidth - 56.0f, panelY + 18.0f, 32.0f, 32.0f})) {
        setVisible(false);
        return;
    }

    if (!connected || !m_hasSnapshot) {
        ui.label(
            "Inventory data is unavailable.",
            glm::vec2(panelX + 24.0f, panelY + 100.0f),
            Color{0.88f, 0.88f, 0.88f, 1.0f}
        );
        return;
    }

    const float slotW = 122.0f;
    const float slotH = 54.0f;
    const float gap = 8.0f;
    const float left = panelX + 24.0f;
    float y = panelY + 86.0f;

    const auto drawSlot = [&](uint16_t slotIndex, float x, float slotY) {
        const Slot &slot = m_slots[slotIndex];
        const bool empty = Inventory::IsEmpty(slot);
        const bool selected = m_selectedSlot == static_cast<int>(slotIndex);
        const bool ammoSlot = Inventory::IsAmmoSlotIndex(slotIndex);
        const bool hotbarSlot = slotIndex < kHotbarSlots;

        const Rect rect{x, slotY, slotW, slotH};
        if (ui.button("", rect)) {
            selectOrMoveSlot(clientNet, slotIndex);
        }

        ui.labelInRect(
            slotTitle(slotIndex),
            Rect{rect.x + 8.0f, rect.y + 4.0f, rect.w - 16.0f, 18.0f},
            Color{0.78f, 0.80f, 0.82f, 1.0f},
            TextAlign::Center,
            TextVerticalAlign::Center
        );
        if (empty) {
            ui.labelInRect(
                "Empty",
                Rect{rect.x + 8.0f, rect.y + 25.0f, rect.w - 16.0f, 22.0f},
                Color{0.50f, 0.50f, 0.52f, 0.92f},
                TextAlign::Center,
                TextVerticalAlign::Center
            );
        } else {
            const TextureHandle blockTexture = ItemIconUi::blockTextureForSlot(*runtime.ui.nativeUi, slot);
            if (blockTexture != 0) {
                ui.image(blockTexture, Rect{rect.x + (rect.w - 26.0f) * 0.5f, rect.y + 23.0f, 26.0f, 26.0f});
            } else {
                const std::string itemName = Ellipsize(slotDisplayName(slotIndex), 13);
                ui.labelInRect(
                    itemName,
                    Rect{rect.x + 8.0f, rect.y + 25.0f, rect.w - 56.0f, 22.0f},
                    Color{0.94f, 0.94f, 0.94f, 1.0f},
                    TextAlign::Start,
                    TextVerticalAlign::Center
                );
            }
            ui.labelInRect(
                "x" + std::to_string(slot.quantity),
                Rect{rect.x + rect.w - 46.0f, rect.y + 25.0f, 38.0f, 22.0f},
                Color{0.92f, 0.92f, 0.92f, 1.0f},
                TextAlign::End,
                TextVerticalAlign::Center
            );
        }

        Color accent{1.0f, 1.0f, 1.0f, 0.10f};
        if (selected) {
            accent = Color{1.0f, 0.48f, 0.0f, 1.0f};
        } else if (ammoSlot) {
            accent = Color{1.0f, 0.72f, 0.40f, 0.78f};
        } else if (hotbarSlot) {
            accent = Color{1.0f, 0.63f, 0.22f, 0.78f};
        }
        ui.panel(Rect{rect.x, rect.y, rect.w, selected ? 3.0f : 2.0f}, accent);
    };

    ui.label("Ammo", glm::vec2(left, y), Color{0.78f, 0.80f, 0.82f, 1.0f});
    y += 24.0f;
    for (uint16_t i = kAmmoStart; i < kAmmoEnd; ++i) {
        drawSlot(i, left + static_cast<float>(i - kAmmoStart) * (slotW + gap), y);
    }

    y += slotH + 30.0f;
    ui.label("Backpack", glm::vec2(left, y), Color{0.78f, 0.80f, 0.82f, 1.0f});
    y += 24.0f;
    for (uint16_t i = kBackpackStart; i < kBackpackEnd; ++i) {
        const uint16_t local = static_cast<uint16_t>(i - kBackpackStart);
        const float x = left + static_cast<float>(local % 6) * (slotW + gap);
        const float rowY = y + static_cast<float>(local / 6) * (slotH + gap);
        drawSlot(i, x, rowY);
    }

    y += (slotH * 2.0f) + gap + 30.0f;
    ui.label("Hotbar", glm::vec2(left, y), Color{0.78f, 0.80f, 0.82f, 1.0f});
    y += 24.0f;
    for (uint16_t i = kHotbarStart; i < kHotbarEnd; ++i) {
        drawSlot(i, left + static_cast<float>(i - kHotbarStart) * (slotW + gap), y);
    }

    const float bottomY = panelY + panelHeight - 82.0f;
    ui.panel(Rect{left, bottomY, 410.0f, 54.0f}, Color{0.07f, 0.075f, 0.085f, 0.90f});
    ui.label("Selected Item", glm::vec2(left + 14.0f, bottomY + 8.0f), Color{0.72f, 0.74f, 0.76f, 1.0f});
    ui.label(
        Ellipsize(selectedLine(), 48),
        glm::vec2(left + 14.0f, bottomY + 28.0f),
        Color{0.94f, 0.94f, 0.94f, 1.0f}
    );

    const bool hasSelection = m_selectedSlot >= 0 && m_selectedSlot < static_cast<int>(kInventorySlotCount);
    const float actionX = left + 430.0f;
    if (ui.iconButton("use_selected", useIcon, Rect{actionX, bottomY + 8.0f, 46.0f, 38.0f}, hasSelection)) {
        useSelected(clientNet);
    }
    if (ui.iconButton(
            "drop_one",
            dropOneIcon,
            Rect{actionX + 56.0f, bottomY + 8.0f, 46.0f, 38.0f},
            hasSelection
        )) {
        dropSelected(clientNet, 1);
    }
    if (ui.iconButton(
            "drop_stack",
            dropStackIcon,
            Rect{actionX + 112.0f, bottomY + 8.0f, 46.0f, 38.0f},
            hasSelection
        )) {
        dropSelected(clientNet, 0);
    }
}
