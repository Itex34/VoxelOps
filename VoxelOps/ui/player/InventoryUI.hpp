#pragma once

#include "../../network/ClientNetwork.hpp"

#include <array>
#include <cstdint>
#include <string>

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
    void selectOrMoveSlot(ClientNetwork &clientNet, uint16_t slotIndex);
    void useSelected(ClientNetwork &clientNet);
    void dropSelected(ClientNetwork &clientNet, uint16_t amount);
    [[nodiscard]] std::string slotDisplayName(uint16_t slotIndex) const;
    [[nodiscard]] std::string slotTitle(uint16_t slotIndex) const;
    [[nodiscard]] std::string selectedLine();
    void clearSelection();
    void drawNative(Runtime &runtime, ClientNetwork &clientNet, bool connected);

    bool m_visible = false;
    bool m_hasSnapshot = false;
    uint32_t m_revision = 0;
    std::array<Slot, kInventorySlotCount> m_slots{};
    int m_selectedSlot = -1;
    uint32_t m_nextRequestId = 1;
    std::string m_selectedItemName;
};
