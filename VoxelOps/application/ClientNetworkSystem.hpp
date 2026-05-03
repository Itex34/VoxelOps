#pragma once

#include "ClientInputIntent.hpp"
#include "../runtime/Runtime.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <array>
#include <cstdint>
#include <functional>

enum class GunType : uint16_t;

struct ClientNetworkSystemContext {
    bool *forceCursorEnabled = nullptr;
    std::function<bool(Runtime &)> beginConnectionAttempt;
    std::function<bool(Runtime &, GunType)> equipGun;
};

class ClientNetworkSystem {
public:
    void update(
        Runtime &runtime, const ClientNetworkSystemContext &ctx, const ClientInputIntent *inputIntent
    );

private:
    void processHotbarSelection(Runtime &runtime);
    void syncEquippedGunFromInventory(Runtime &runtime, const ClientNetworkSystemContext &ctx);

    std::array<bool, kHotbarSlots> m_wasHotbarSelectPressed{};
};
