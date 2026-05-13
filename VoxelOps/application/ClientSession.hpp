#pragma once

#include "ClientInputIntent.hpp"
#include "ClientDisconnectReset.hpp"
#include "../runtime/Runtime.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <array>
#include <cstdint>
#include <functional>

enum class GunType : uint16_t;

struct ClientSessionContext {
    bool *forceCursorEnabled = nullptr;
    std::function<bool(Runtime &)> beginConnectionAttempt;
    std::function<bool(Runtime &, GunType)> equipGun;
};

class ClientSession {
public:
    void update(
        Runtime &runtime, const ClientSessionContext &ctx, const ClientInputIntent *inputIntent
    );

private:
    void processHotbarSelection(Runtime &runtime);
    void syncEquippedGunFromInventory(Runtime &runtime, const ClientSessionContext &ctx);

    std::array<bool, kHotbarSlots> m_wasHotbarSelectPressed{};
    ClientDisconnectReset m_disconnectReset;
};
