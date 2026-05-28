#include "InventoryUI.hpp"

#include "../../../Shared/items/Items.hpp"
#include "../../../Shared/runtime/Paths.hpp"
#include "../../runtime/Runtime.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
    template <size_t N>
    void SetButtonText(Rml::Element *button, const std::array<char, N> &text) {
        if (!button) {
            return;
        }
        button->SetInnerRML(text.data());
    }

    std::string BuildSlotButtonsRml(uint16_t start, uint16_t end, std::string_view labelPrefix) {
        std::string rml;
        for (uint16_t i = start; i < end; ++i) {
            const uint16_t localIndex = static_cast<uint16_t>(i - start + 1);
            rml += "<button class='slot_btn' id='slot_btn_";
            rml += std::to_string(i);
            rml += "'>";
            rml += std::string(labelPrefix);
            rml += " ";
            rml += std::to_string(localIndex);
            rml += "<br/>Empty</button>";
        }
        return rml;
    }
} // namespace

class InventoryUI::RmlInventoryListener final : public Rml::EventListener {
public:
    explicit RmlInventoryListener(InventoryUI *owner)
        : m_owner(owner) {}

    void setFrameContext(Runtime *runtime, ClientNetwork *clientNet) {
        m_runtime = runtime;
        m_clientNet = clientNet;
    }

    void ProcessEvent(Rml::Event &event) override {
        if (!m_owner || !m_runtime || !m_clientNet) {
            return;
        }
        m_owner->handleRmlEvent(*m_runtime, *m_clientNet, event);
    }

private:
    InventoryUI *m_owner = nullptr;
    Runtime *m_runtime = nullptr;
    ClientNetwork *m_clientNet = nullptr;
};

InventoryUI::InventoryUI() = default;
InventoryUI::~InventoryUI() = default;

void InventoryUI::setVisible(bool visible) noexcept {
    m_visible = visible;
    if (!m_visible) {
        m_selectedSlot = -1;
        m_selectedItemName.clear();
        if (m_rmlDocument && m_rmlDocumentVisible) {
            m_rmlDocument->Hide();
            m_rmlDocumentVisible = false;
        }
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
    m_selectedSlot = -1;
    m_selectedItemName.clear();
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

void InventoryUI::draw(Runtime &runtime, ClientNetwork &clientNet, bool connected) {
    if (!m_visible) {
        return;
    }

    if (runtime.ui.rmlUi && runtime.ui.rmlUi->isUsingOpenGlBackend()) {
        drawRml(runtime, clientNet, connected);
        return;
    }

    drawImGui(runtime, clientNet, connected);
}

bool InventoryUI::ensureRmlDocument(Runtime &runtime) {
    if (!runtime.ui.rmlUi || !runtime.ui.rmlUi->isUsingOpenGlBackend()) {
        if (m_rmlDocument) {
            resetRmlDocument();
        } else {
            forgetRmlState();
        }
        return false;
    }

    Rml::Context *context = runtime.ui.rmlUi->context();
    if (context == nullptr) {
        forgetRmlState();
        return false;
    }
    if (m_rmlContext == context && m_rmlDocument != nullptr) {
        return true;
    }

    if (m_rmlContext != nullptr && m_rmlContext != context) {
        forgetRmlState();
    } else {
        resetRmlDocument();
    }
    m_rmlContext = context;

    const std::string inventoryPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("ui/rml/documents/inventory.rml").generic_string();
    m_rmlDocument = m_rmlContext->LoadDocument(inventoryPath);
    if (!m_rmlDocument) {
        return false;
    }

    m_backdrop = m_rmlDocument->GetElementById("inventory_backdrop");
    m_panel = m_rmlDocument->GetElementById("inventory_panel");
    m_statusText = m_rmlDocument->GetElementById("status_line");
    m_selectedText = m_rmlDocument->GetElementById("selected_line");
    m_useButton = m_rmlDocument->GetElementById("use_btn");
    m_dropOneButton = m_rmlDocument->GetElementById("drop1_btn");
    m_dropStackButton = m_rmlDocument->GetElementById("dropstack_btn");
    Rml::Element *ammoGrid = m_rmlDocument->GetElementById("ammo_grid");
    Rml::Element *bagGrid = m_rmlDocument->GetElementById("bag_grid");
    Rml::Element *hotbarGrid = m_rmlDocument->GetElementById("hotbar_grid");
    if (!m_backdrop || !m_panel || !m_statusText || !m_selectedText || !m_useButton ||
        !m_dropOneButton || !m_dropStackButton || !ammoGrid || !bagGrid || !hotbarGrid) {
        resetRmlDocument();
        return false;
    }

    constexpr uint16_t kHotbarStart = 0;
    constexpr uint16_t kHotbarEnd = kHotbarStart + kHotbarSlots;
    constexpr uint16_t kBackpackStart = kHotbarEnd;
    constexpr uint16_t kBackpackEnd = kBackpackStart + kBackpackSlots;
    constexpr uint16_t kAmmoStart = kBackpackEnd;
    constexpr uint16_t kAmmoEnd = kAmmoStart + kAmmoSlots;

    ammoGrid->SetInnerRML(BuildSlotButtonsRml(kAmmoStart, kAmmoEnd, "Ammo"));
    bagGrid->SetInnerRML(BuildSlotButtonsRml(kBackpackStart, kBackpackEnd, "Bag"));
    hotbarGrid->SetInnerRML(BuildSlotButtonsRml(kHotbarStart, kHotbarEnd, "Hotbar"));

    for (uint16_t i = 0; i < kInventorySlotCount; ++i) {
        m_slotButtons[i] = m_rmlDocument->GetElementById("slot_btn_" + std::to_string(i));
    }

    m_rmlListener = std::make_unique<RmlInventoryListener>(this);
    m_backdrop->AddEventListener("click", m_rmlListener.get());
    m_useButton->AddEventListener("click", m_rmlListener.get());
    m_dropOneButton->AddEventListener("click", m_rmlListener.get());
    m_dropStackButton->AddEventListener("click", m_rmlListener.get());
    for (Rml::Element *button : m_slotButtons) {
        if (button) {
            button->AddEventListener("click", m_rmlListener.get());
        }
    }

    m_rmlDocument->Show();
    m_rmlDocumentVisible = true;
    return true;
}

void InventoryUI::resetRmlDocument() {
    if (m_backdrop && m_rmlListener) {
        m_backdrop->RemoveEventListener("click", m_rmlListener.get());
    }
    if (m_useButton && m_rmlListener) {
        m_useButton->RemoveEventListener("click", m_rmlListener.get());
    }
    if (m_dropOneButton && m_rmlListener) {
        m_dropOneButton->RemoveEventListener("click", m_rmlListener.get());
    }
    if (m_dropStackButton && m_rmlListener) {
        m_dropStackButton->RemoveEventListener("click", m_rmlListener.get());
    }
    if (m_rmlListener) {
        for (Rml::Element *button : m_slotButtons) {
            if (button) {
                button->RemoveEventListener("click", m_rmlListener.get());
            }
        }
    }
    if (m_rmlDocument) {
        m_rmlDocument->Close();
    }
    forgetRmlState();
}

void InventoryUI::forgetRmlState() {
    m_rmlListener.reset();
    m_backdrop = nullptr;
    m_panel = nullptr;
    m_statusText = nullptr;
    m_selectedText = nullptr;
    m_useButton = nullptr;
    m_dropOneButton = nullptr;
    m_dropStackButton = nullptr;
    m_slotButtons.fill(nullptr);
    m_rmlDocument = nullptr;
    m_rmlContext = nullptr;
    m_rmlDocumentVisible = false;
}

void InventoryUI::syncRmlState(Runtime &, bool connected) {
    if (!m_rmlDocument || !m_statusText) {
        return;
    }

    if (!connected) {
        m_statusText->SetInnerRML("Not connected.");
    } else if (!m_hasSnapshot) {
        m_statusText->SetInnerRML("Waiting for inventory...");
    } else {
        m_statusText->SetInnerRML("Inventory ready.");
    }

    constexpr uint16_t kHotbarStart = 0;
    constexpr uint16_t kHotbarEnd = kHotbarStart + kHotbarSlots;
    constexpr uint16_t kBackpackStart = kHotbarEnd;
    constexpr uint16_t kBackpackEnd = kBackpackStart + kBackpackSlots;
    constexpr uint16_t kAmmoStart = kBackpackEnd;
    constexpr uint16_t kAmmoEnd = kAmmoStart + kAmmoSlots;

    for (uint16_t slotIndex = 0; slotIndex < kInventorySlotCount; ++slotIndex) {
        Rml::Element *button = m_slotButtons[slotIndex];
        if (!button) {
            continue;
        }

        const Slot &slot = m_slots[slotIndex];
        const bool empty = Inventory::IsEmpty(slot);
        std::string itemName = "Empty";
        if (!empty && Inventory::IsValidItemId(slot.itemId)) {
            itemName = Items::ItemDatabase[slot.itemId].name;
            if (itemName.empty()) {
                itemName = "Item " + std::to_string(slot.itemId);
            }
        }

        std::string title = "Slot";
        if (slotIndex >= kAmmoStart && slotIndex < kAmmoEnd) {
            title = "Ammo " + std::to_string(slotIndex - kAmmoStart + 1);
        } else if (slotIndex >= kBackpackStart && slotIndex < kBackpackEnd) {
            title = "Bag " + std::to_string(slotIndex - kBackpackStart + 1);
        } else {
            title = "Hotbar " + std::to_string(slotIndex - kHotbarStart + 1);
        }

        std::string label = title + "<br/>" + itemName;
        if (!empty) {
            label += " x" + std::to_string(slot.quantity);
        }
        button->SetInnerRML(label);

        const bool selected = (m_selectedSlot == static_cast<int>(slotIndex));
        const bool ammoSlot = Inventory::IsAmmoSlotIndex(slotIndex);
        const bool hotbarSlot = slotIndex < kHotbarSlots;
        if (selected) {
            button->SetProperty("background-color", "#1c1c1c");
            button->SetProperty("border", "2px #ff7b00");
        } else if (ammoSlot) {
            button->SetProperty("background-color", "#242424");
            button->SetProperty("border", "1px #ffb866");
        } else if (hotbarSlot) {
            button->SetProperty("background-color", "#242424");
            button->SetProperty("border", "1px #ff9f43");
        } else {
            button->SetProperty("background-color", "#242424");
            button->SetProperty("border", "1px #4a4a4a");
        }
    }

    if (m_selectedText) {
        if (m_selectedSlot >= 0 && m_selectedSlot < static_cast<int>(kInventorySlotCount)) {
            const uint16_t slotIndex = static_cast<uint16_t>(m_selectedSlot);
            const Slot &slot = m_slots[slotIndex];
            if (!Inventory::IsEmpty(slot)) {
                if (m_selectedItemName.empty() && Inventory::IsValidItemId(slot.itemId)) {
                    m_selectedItemName = Items::ItemDatabase[slot.itemId].name;
                }
                m_selectedText->SetInnerRML(
                    "Slot " + std::to_string(slotIndex) + ": " + m_selectedItemName + " x" +
                    std::to_string(slot.quantity)
                );
            } else {
                m_selectedSlot = -1;
                m_selectedItemName.clear();
                m_selectedText->SetInnerRML("No slot selected.");
            }
        } else {
            m_selectedText->SetInnerRML("No slot selected.");
        }
    }

    auto setDisabled = [connected, this](Rml::Element *button) {
        if (!button) {
            return;
        }
        if (!connected || !m_hasSnapshot || m_selectedSlot < 0) {
            button->SetAttribute("disabled", "");
        } else {
            button->RemoveAttribute("disabled");
        }
    };
    setDisabled(m_useButton);
    setDisabled(m_dropOneButton);
    setDisabled(m_dropStackButton);
}

void InventoryUI::handleRmlEvent(Runtime &, ClientNetwork &clientNet, Rml::Event &event) {
    if (!m_visible || event.GetType() != "click") {
        return;
    }

    Rml::Element *source = event.GetCurrentElement();
    if (!source) {
        return;
    }

    if (source == m_backdrop && event.GetTargetElement() == m_backdrop) {
        m_visible = false;
        m_selectedSlot = -1;
        m_selectedItemName.clear();
        return;
    }

    if (source == m_useButton || source == m_dropOneButton || source == m_dropStackButton) {
        if (m_selectedSlot < 0 || m_selectedSlot >= static_cast<int>(kInventorySlotCount)) {
            return;
        }
        const uint16_t slotIndex = static_cast<uint16_t>(m_selectedSlot);
        if (source == m_useButton) {
            (void)submitAction(clientNet, InventoryActionType::Use, slotIndex, slotIndex, 1);
        } else if (source == m_dropOneButton) {
            (void)submitAction(clientNet, InventoryActionType::Drop, slotIndex, slotIndex, 1);
        } else {
            (void)submitAction(clientNet, InventoryActionType::Drop, slotIndex, slotIndex, 0);
        }
        return;
    }

    const std::string id = source->GetId();
    constexpr std::string_view prefix = "slot_btn_";
    if (!id.starts_with(prefix)) {
        return;
    }
    const uint16_t slotIndex = static_cast<uint16_t>(
        std::stoi(id.substr(prefix.size()))
    );
    if (slotIndex >= kInventorySlotCount) {
        return;
    }

    const Slot &slot = m_slots[slotIndex];
    const bool empty = Inventory::IsEmpty(slot);
    std::string itemName = "Empty";
    if (!empty && Inventory::IsValidItemId(slot.itemId)) {
        itemName = Items::ItemDatabase[slot.itemId].name;
        if (itemName.empty()) {
            itemName = "Item " + std::to_string(slot.itemId);
        }
    }

    if (m_selectedSlot < 0) {
        if (!empty) {
            m_selectedSlot = static_cast<int>(slotIndex);
            m_selectedItemName = itemName;
        }
        return;
    }
    if (m_selectedSlot == static_cast<int>(slotIndex)) {
        m_selectedSlot = -1;
        m_selectedItemName.clear();
        return;
    }

    const uint16_t sourceSlot = static_cast<uint16_t>(m_selectedSlot);
    const Slot &sourceSlotData = m_slots[sourceSlot];
    if (Inventory::IsEmpty(sourceSlotData)) {
        m_selectedSlot = -1;
        m_selectedItemName.clear();
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
        m_selectedSlot = -1;
        m_selectedItemName.clear();
    }
}

void InventoryUI::drawRml(Runtime &runtime, ClientNetwork &clientNet, bool connected) {
    if (!ensureRmlDocument(runtime)) {
        drawImGui(runtime, clientNet, connected);
        return;
    }

    if (m_rmlListener) {
        m_rmlListener->setFrameContext(&runtime, &clientNet);
    }

    if (m_visible && m_rmlDocument && !m_rmlDocumentVisible) {
        m_rmlDocument->Show();
        m_rmlDocumentVisible = true;
    }
    if (!m_visible && m_rmlDocument && m_rmlDocumentVisible) {
        m_rmlDocument->Hide();
        m_rmlDocumentVisible = false;
    }
    if (!m_visible) {
        return;
    }

    syncRmlState(runtime, connected);
}

void InventoryUI::drawImGui(Runtime &, ClientNetwork &clientNet, bool connected) {
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 windowSize(760.0f, 520.0f);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f)
    );
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
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

    const bool hoveredInventoryWindow =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    const bool clickOutside = (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Right)) &&
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

    const auto drawSlotButton = [&](const uint16_t slotIndex, const std::string &slotTitle) {
        const Slot &slot = m_slots[slotIndex];
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
        } else if (ammoSlot) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.25f, 0.12f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.37f, 0.33f, 0.16f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.21f, 0.10f, 0.95f));
        } else if (hotbarSlot) {
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
            } else if (m_selectedSlot == static_cast<int>(slotIndex)) {
                m_selectedSlot = -1;
                m_selectedItemName.clear();
            } else {
                const uint16_t sourceSlot = static_cast<uint16_t>(m_selectedSlot);
                const Slot &source = m_slots[sourceSlot];
                if (Inventory::IsEmpty(source)) {
                    m_selectedSlot = -1;
                    m_selectedItemName.clear();
                } else if (!Inventory::IsItemAllowedInSlot(source.itemId, slotIndex)) {
                } else {
                    InventoryActionType actionType = InventoryActionType::Move;
                    bool canSubmit = true;
                    if (!empty && slot.itemId != source.itemId) {
                        if (!Inventory::IsItemAllowedInSlot(slot.itemId, sourceSlot)) {
                            canSubmit = false;
                        } else {
                            actionType = InventoryActionType::Swap;
                        }
                    }
                    if (canSubmit &&
                        submitAction(clientNet, actionType, sourceSlot, slotIndex, 0)) {
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
        const Slot &slot = m_slots[slotIndex];
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
        } else {
            m_selectedSlot = -1;
            m_selectedItemName.clear();
            ImGui::TextUnformatted("No slot selected.");
        }
    }
    ImGui::End();
}
