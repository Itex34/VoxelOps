#include "BroadcastService.hpp"

#include "../core/LockWaitTelemetry.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <vector>

BroadcastService::BroadcastService(std::mutex &mutex, ClientSessionManager &sessions)
    : m_mutex(mutex)
    , m_sessions(sessions) {}

void BroadcastService::BroadcastRaw(
    const void *data, uint32_t len, HSteamNetConnection except
) const {
    std::vector<HSteamNetConnection> recipients;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(m_mutex, "BroadcastService::BroadcastRaw");
        recipients.reserve(m_sessions.size());
        for (const auto &[conn, _] : m_sessions) {
            recipients.push_back(conn);
        }
    }
    for (HSteamNetConnection conn : recipients) {
        if (conn == except) {
            continue;
        }
        SteamNetworkingSockets()->SendMessageToConnection(
            conn, data, len, k_nSteamNetworkingSend_Reliable, nullptr
        );
    }
}
