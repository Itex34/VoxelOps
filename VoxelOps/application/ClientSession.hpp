#pragma once

#include "ClientInputIntent.hpp"
#include "ClientDisconnectReset.hpp"
#include "FrameServices.hpp"
#include "../runtime/Runtime.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <array>
#include <cstdint>

enum class GunType : uint16_t;

struct ClientSessionContext {
    bool *forceCursorEnabled = nullptr;
    FrameConnectionHost *connectionHost = nullptr;
};

class ClientSession {
public:
    void
    update(Runtime &runtime, const ClientSessionContext &ctx, const ClientInputIntent *inputIntent);
    bool sendPredictedInputTick(
        Runtime &runtime,
        const ClientInputIntent &inputIntent,
        double deltaSeconds,
        bool sendRedundantInputs = true
    );

private:
    void processHotbarSelection(Runtime &runtime);
    void syncEquippedGunFromInventory(Runtime &runtime, const ClientSessionContext &ctx);

    std::array<bool, kHotbarSlots> m_wasHotbarSelectPressed{};
    ClientDisconnectReset m_disconnectReset;
};
