#include "NetworkRuntime.hpp"

#include <iostream>

NetworkRuntime::NetworkRuntime() = default;

NetworkRuntime::~NetworkRuntime() {
    Shutdown();
}

bool NetworkRuntime::Start(
    uint16_t port,
    void (*connectionStatusChangedCallback)(SteamNetConnectionStatusChangedCallback_t *),
    bool &boundAddressAvailable,
    SteamNetworkingIPAddr &boundAddress
) {
    boundAddressAvailable = false;
    if (m_started) {
        return true;
    }
    if (connectionStatusChangedCallback == nullptr) {
        std::cerr << "Connection status callback is required\n";
        return false;
    }
    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        std::cerr << "GameNetworkingSockets_Init failed: " << err << "\n";
        return false;
    }

    m_pollGroup = SteamNetworkingSockets()->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid) {
        std::cerr << "CreatePollGroup failed\n";
        GameNetworkingSockets_Kill();
        return false;
    }

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(
        k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void *>(connectionStatusChangedCallback)
    );

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;
    m_listenSock = SteamNetworkingSockets()->CreateListenSocketIP(addr, 1, &opt);
    if (m_listenSock == k_HSteamListenSocket_Invalid) {
        std::cerr << "CreateListenSocketIP failed\n";
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
        GameNetworkingSockets_Kill();
        return false;
    }

    boundAddressAvailable = SteamNetworkingSockets()->GetListenSocketAddress(m_listenSock, &boundAddress);
    m_started = true;
    return true;
}

void NetworkRuntime::Shutdown() {
    if (!m_started) {
        return;
    }

    if (m_listenSock != k_HSteamListenSocket_Invalid) {
        SteamNetworkingSockets()->CloseListenSocket(m_listenSock);
        m_listenSock = k_HSteamListenSocket_Invalid;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    m_started = false;
    GameNetworkingSockets_Kill();
}

HSteamNetPollGroup &NetworkRuntime::PollGroupRef() {
    return m_pollGroup;
}
