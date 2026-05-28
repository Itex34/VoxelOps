#pragma once

#include "../../../Shared/network/Packets.hpp"
#include "../../../Shared/player/PlayerID.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "../session/ClientSessionManager.hpp"
#include "WorldItemService.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <functional>
#include <mutex>

class BlockEditRequestService {
public:
    struct Hooks {
        std::function<BlockPlaceResult(PlayerID, const BlockPlaceRequest &)> executeBlockPlaceRequest;
        std::function<BlockBreakResult(PlayerID, const BlockBreakRequest &)> executeBlockBreakRequest;
    };

    BlockEditRequestService(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        WorldItemService &worldItemService,
        Hooks hooks
    );

    void HandleBlockPlaceRequestPacket(HSteamNetConnection incoming, const void *data, uint32_t size);
    void HandleBlockBreakRequestPacket(HSteamNetConnection incoming, const void *data, uint32_t size);

private:
    bool TryGetRegisteredPlayerId(HSteamNetConnection incoming, PlayerID &outPlayerId);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    WorldItemService &m_worldItemService;
    Hooks m_hooks;
};
