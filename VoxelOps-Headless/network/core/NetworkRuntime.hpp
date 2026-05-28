#pragma once

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();

    NetworkRuntime(const NetworkRuntime &) = delete;
    NetworkRuntime &operator=(const NetworkRuntime &) = delete;

    bool Start(
        uint16_t port,
        void (*connectionStatusChangedCallback)(SteamNetConnectionStatusChangedCallback_t *),
        bool &boundAddressAvailable,
        SteamNetworkingIPAddr &boundAddress
    );
    void Shutdown();

    HSteamNetPollGroup &PollGroupRef();

private:
    bool m_started = false;
    HSteamNetPollGroup m_pollGroup = k_HSteamNetPollGroup_Invalid;
    HSteamListenSocket m_listenSock = k_HSteamListenSocket_Invalid;
};
