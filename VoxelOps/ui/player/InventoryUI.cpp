#include "InventoryUI.hpp"

#include "../../../Shared/items/Items.hpp"

#include <imgui.h>

#include <algorithm>

void InventoryUI::setVisible(bool visible) noexcept {
    m_visible = visible;
    if (!m_visible) {
        m_selectedSlot = -1;
        m_selectedItemName.clear();
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

const std::array<Slot, kInventorySlotCount>& InventoryUI::slots() const noexcept {
    return m_slots;
}

uint32_t InventoryUI::revision() const noexcept {
    return m_revision;
}

void InventoryUI::reset() {
    m_hasSnapshot = false;
    m_revision = 0;
    m_selectedSlot = -1;
    m_selectedItemName.clear();
    for (Slot& slot : m_slots) {
        slot.itemId = kInventoryEmptyItemId;
        slot.quantity = 0;
    }
}

bool InventoryUI::hasAuthoritativeSnapshot() const noexcept {
    return m_hasSnapshot;
}

bool InventoryUI::submitAction(
    ClientNetwork& clientNet,
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

void InventoryUI::consumeNetwork(ClientNetwork& clientNet) {
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
            const Slot& selected = m_slots[static_cast<size_t>(m_selectedSlot)];
            if (Inventory::IsEmpty(selected)) {
                m_selectedSlot = -1;
                m_selectedItemName.clear();
            }
        }
    }

    InventoryActionResult result{};
    while (clientNet.PopInventoryActionResult(result)) {
        (void)result;
    }
}

void InventoryUI::draw(ClientNetwork& clientNet, bool connected) {
    if (!m_visible) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 windowSize(760.0f, 520.0f);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f)
    );
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Inventory", nullptr, windowFlags)) {
        ImGui::End();
        return;
    }

    if (!connected) {
        ImGui::TextUnformatted("Not connected.");
        ImGui::End();
        return;
    }

    if (!m_hasSnapshot) {
        ImGui::TextUnformatted("Waiting for inventory...");
        ImGui::End();
        return;
    }

    const bool hoveredInventoryWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const bool clickOutside =
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) &&
        !hoveredInventoryWindow;
    if (clickOutside) {
        m_visible = false;
        m_selectedSlot = -1;
        m_selectedItemName.clear();
        ImGui::End();
        return;
    }

    constexpr ImVec2 kSlotButtonSize(112.0f, 56.0f);
    constexpr uint16_t kHotbarStart = 0;
    constexpr uint16_t kHotbarEnd = kHotbarStart + kHotbarSlots;
    constexpr uint16_t kBackpackStart = kHotbarEnd;
    constexpr uint16_t kBackpackEnd = kBackpackStart + kBackpackSlots;
    constexpr uint16_t kAmmoStart = kBackpackEnd;
    constexpr uint16_t kAmmoEnd = kAmmoStart + kAmmoSlots;

    const auto drawSlotButton = [&](const uint16_t slotIndex, const std::string& slotTitle) {
        const Slot& slot = m_slots[slotIndex];
        const bool empty = Inventory::IsEmpty(slot);
        const bool selected = (m_selectedSlot == static_cast<int>(slotIndex));
        const bool ammoSlot = Inventory::IsAmmoSlotIndex(slotIndex);
        const bool hotbarSlot = slotIndex < kHotbarSlots;

        std::string itemName = "Empty";
        if (!empty && Inventory::IsValidItemId(slot.itemId)) {
            itemName = Items::ItemDatabase[slot.itemId].name;
            if (itemName.empty()) {
                itemName = "Item " + std::to_string(slot.itemId);
            }
        }

        std::string buttonLabel = slotTitle;
        buttonLabel += "\n";
        buttonLabel += itemName;
        if (!empty) {
            buttonLabel += " x";
            buttonLabel += std::to_string(slot.quantity);
        }

        ImGui::PushID(static_cast<int>(slotIndex));
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.58f, 0.31f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.40f, 0.22f, 1.0f));
        }
        else if (ammoSlot) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.25f, 0.12f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.37f, 0.33f, 0.16f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.21f, 0.10f, 0.95f));
        }
        else if (hotbarSlot) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.18f, 0.32f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.24f, 0.42f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.16f, 0.28f, 0.95f));
        }

        const bool clicked = ImGui::Button(buttonLabel.c_str(), kSlotButtonSize);
        if (selected || ammoSlot || hotbarSlot) {
            ImGui::PopStyleColor(3);
        }

        if (clicked) {
            if (m_selectedSlot < 0) {
                if (!empty) {
                    m_selectedSlot = static_cast<int>(slotIndex);
                    m_selectedItemName = itemName;
                }
            }
            else if (m_selectedSlot == static_cast<int>(slotIndex)) {
                m_selectedSlot = -1;
                m_selectedItemName.clear();
            }
            else {
                const uint16_t sourceSlot = static_cast<uint16_t>(m_selectedSlot);
                const Slot& source = m_slots[sourceSlot];
                if (Inventory::IsEmpty(source)) {
                    m_selectedSlot = -1;
                    m_selectedItemName.clear();
                }
                else if (!Inventory::IsItemAllowedInSlot(source.itemId, slotIndex)) {
                    // Destination cannot accept this item (for example non-ammo into ammo slot).
                }
                else {
                    InventoryActionType actionType = InventoryActionType::Move;
                    bool canSubmit = true;
                    if (!empty && slot.itemId != source.itemId) {
                        if (!Inventory::IsItemAllowedInSlot(slot.itemId, sourceSlot)) {
                            canSubmit = false;
                        }
                        else {
                            actionType = InventoryActionType::Swap;
                        }
                    }
                    if (canSubmit && submitAction(clientNet, actionType, sourceSlot, slotIndex, 0)) {
                        m_selectedSlot = -1;
                        m_selectedItemName.clear();
                    }
                }
            }
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !empty) {
            (void)submitAction(clientNet, InventoryActionType::Drop, slotIndex, slotIndex, 1);
        }

        ImGui::PopID();
    };

    ImGui::SeparatorText("Ammo (Top, Ammo-Only)");
    for (uint16_t i = kAmmoStart; i < kAmmoEnd; ++i) {
        const uint16_t ammoIndex = static_cast<uint16_t>(i - kAmmoStart + 1);
        drawSlotButton(i, "Ammo " + std::to_string(ammoIndex));
        if ((i + 1) < kAmmoEnd) {
            ImGui::SameLine();
        }
    }

    ImGui::SeparatorText("Backpack");
    constexpr int kBackpackColumns = 6;
    for (uint16_t i = kBackpackStart; i < kBackpackEnd; ++i) {
        const uint16_t backpackIndex = static_cast<uint16_t>(i - kBackpackStart + 1);
        drawSlotButton(i, "Bag " + std::to_string(backpackIndex));
        const uint16_t localIndex = static_cast<uint16_t>(i - kBackpackStart);
        if (((localIndex + 1) % kBackpackColumns) != 0 && (i + 1) < kBackpackEnd) {
            ImGui::SameLine();
        }
    }

    ImGui::SeparatorText("Hotbar (Bottom)");
    constexpr int kHotbarColumns = 6;
    for (uint16_t i = kHotbarStart; i < kHotbarEnd; ++i) {
        const uint16_t hotbarIndex = static_cast<uint16_t>(i - kHotbarStart + 1);
        drawSlotButton(i, "[" + std::to_string(hotbarIndex) + "] Hotbar");
        const uint16_t localIndex = static_cast<uint16_t>(i - kHotbarStart);
        if (((localIndex + 1) % kHotbarColumns) != 0 && (i + 1) < kHotbarEnd) {
            ImGui::SameLine();
        }
    }

    ImGui::SeparatorText("Selected");
    if (m_selectedSlot >= 0 && m_selectedSlot < static_cast<int>(kInventorySlotCount)) {
        const uint16_t slotIndex = static_cast<uint16_t>(m_selectedSlot);
        const Slot& slot = m_slots[slotIndex];
        if (!Inventory::IsEmpty(slot)) {
            if (m_selectedItemName.empty() && Inventory::IsValidItemId(slot.itemId)) {
                m_selectedItemName = Items::ItemDatabase[slot.itemId].name;
            }
            ImGui::Text("Slot %u: %s x%u", slotIndex, m_selectedItemName.c_str(), slot.quantity);

            if (ImGui::Button("Use Item")) {
                (void)submitAction(clientNet, InventoryActionType::Use, slotIndex, slotIndex, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Drop 1")) {
                (void)submitAction(clientNet, InventoryActionType::Drop, slotIndex, slotIndex, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Drop Stack")) {
                (void)submitAction(clientNet, InventoryActionType::Drop, slotIndex, slotIndex, 0);
            }
        }
        else {
            m_selectedSlot = -1;
            m_selectedItemName.clear();
            ImGui::TextUnformatted("No slot selected.");
        }
    }
    ImGui::End();
}

