#pragma once

#include "../../../Shared/network/PacketType.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <functional>

class NetworkPump {
public:
    struct Hooks {
        std::function<bool(HSteamNetConnection, PacketType, uint32_t)> isInboundRateLimitExceeded;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onConnectRequest;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onMessage;
        std::function<void(HSteamNetConnection, const void *, uint32_t, uint64_t &)> onPlayerInput;
        std::function<void(HSteamNetConnection, const void *, uint32_t, uint64_t &)>
            onChunkRequest;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onBlockPlaceRequest;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onBlockBreakRequest;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onShootRequest;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onGrappleRequest;
        std::function<void(HSteamNetConnection, const void *, uint32_t)> onInventoryActionRequest;
    };

    NetworkPump(HSteamNetPollGroup &pollGroup, Hooks hooks);

    void PumpInbound(
        uint64_t &msgPacketsThisLoop,
        uint64_t &playerInputPacketsThisLoop,
        uint64_t &chunkRequestPacketsThisLoop,
        double &messageDrainUs
    );

private:
    void DispatchInboundPacket(
        HSteamNetConnection incoming,
        PacketType packetType,
        const void *data,
        uint32_t size,
        uint64_t &playerInputPacketsThisLoop,
        uint64_t &chunkRequestPacketsThisLoop
    );

private:
    HSteamNetPollGroup &m_pollGroup;
    Hooks m_hooks;
};
