#pragma once

#include "../runtime/Runtime.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>

enum class GunType : uint16_t;

struct ClientNetworkSystemContext {
    SDL_Window *window = nullptr;
    bool *forceCursorEnabled = nullptr;
    std::function<bool(Runtime &)> beginConnectionAttempt;
    std::function<bool(Runtime &, GunType)> equipGun;
};

class ClientNetworkSystem {
  public:
    void update(Runtime &runtime, const ClientNetworkSystemContext &ctx);

  private:
    void processHotbarSelection(Runtime &runtime);
    void syncEquippedGunFromInventory(Runtime &runtime, const ClientNetworkSystemContext &ctx);

    std::array<bool, kHotbarSlots> m_wasHotbarSelectPressed{};
};
