#pragma once

#include "../../../Shared/network/Packets.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <functional>

class CombatRequestService {
public:
    struct Hooks {
        std::function<ShootResult(HSteamNetConnection, const ShootRequest &)> executeShootRequest;
        std::function<GrappleResult(HSteamNetConnection, const GrappleRequest &)>
            executeGrappleRequest;
    };

    explicit CombatRequestService(Hooks hooks);

    void HandleShootRequestPacket(HSteamNetConnection incoming, const void *data, uint32_t size);
    void HandleGrappleRequestPacket(HSteamNetConnection incoming, const void *data, uint32_t size);

private:
    Hooks m_hooks;
};
