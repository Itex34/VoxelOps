#pragma once

#include "../../../Shared/network/PacketType.hpp"
#include "NetworkPump.hpp"
#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class TickNetworkPhase {
public:
    using ClientSession = ClientSessionManager::ClientSession;

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
        std::function<void(HSteamNetConnection, const ClientSession &, const char *, bool)>
            teardownClientSession;
    };

    TickNetworkPhase(
        std::mutex &mutex, ClientSessionManager &sessions, HSteamNetPollGroup &pollGroup, Hooks hooks
    );

    void RunInboundMessagePhase(
        uint64_t &msgPacketsThisLoop,
        uint64_t &playerInputPacketsThisLoop,
        uint64_t &chunkRequestPacketsThisLoop,
        double &messageDrainUs
    );
    void RunConnectionCleanupPhase();

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    Hooks m_hooks;
    NetworkPump m_networkPump;
};
