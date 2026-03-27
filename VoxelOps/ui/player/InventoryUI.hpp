#pragma once

#include "../../network/ClientNetwork.hpp"

#include <array>
#include <cstdint>
#include <string>

class InventoryUI {
public:
    void setVisible(bool visible) noexcept;
    void toggleVisible() noexcept;
    [[nodiscard]] bool isVisible() const noexcept;

    void reset();
    void consumeNetwork(ClientNetwork& clientNet);
    void draw(ClientNetwork& clientNet, bool connected);
    [[nodiscard]] bool hasSnapshot() const noexcept;
    [[nodiscard]] const std::array<Slot, kInventorySlotCount>& slots() const noexcept;
    [[nodiscard]] uint32_t revision() const noexcept;

private:
    [[nodiscard]] bool hasAuthoritativeSnapshot() const noexcept;
    bool submitAction(
        ClientNetwork& clientNet,
        InventoryActionType type,
        uint16_t sourceSlot,
        uint16_t destinationSlot,
        uint16_t amount
    );

    bool m_visible = false;
    bool m_hasSnapshot = false;
    uint32_t m_revision = 0;
    std::array<Slot, kInventorySlotCount> m_slots{};
    int m_selectedSlot = -1;
    uint32_t m_nextRequestId = 1;
    std::string m_selectedItemName;
};
