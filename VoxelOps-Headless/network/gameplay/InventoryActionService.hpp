#pragma once

#include "../../game/player/PlayerManager.hpp"
#include "../session/ClientSessionManager.hpp"
#include "WorldItemService.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <mutex>

class InventoryActionService {
public:
    InventoryActionService(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        WorldItemService &worldItemService
    );

    void HandleInventoryActionRequestPacket(HSteamNetConnection incoming, const void *data, uint32_t size);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    WorldItemService &m_worldItemService;
};
