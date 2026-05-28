#pragma once

#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <mutex>

class BroadcastService {
public:
    BroadcastService(std::mutex &mutex, ClientSessionManager &sessions);

    void BroadcastRaw(
        const void *data, uint32_t len, HSteamNetConnection except = k_HSteamNetConnection_Invalid
    ) const;

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
};
