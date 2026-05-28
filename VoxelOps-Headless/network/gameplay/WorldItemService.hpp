#pragma once

#include "../../../Shared/network/Packets.hpp"
#include "../../../Shared/player/PlayerID.hpp"
#include "../../engine/physics/simulation/WorldItemPhysics.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

class WorldItemService {
public:
    WorldItemService(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        ChunkManager &chunkManager
    );

    void Reset();
    void SpawnDroppedItem(PlayerID dropperId, uint16_t itemId, uint16_t quantity);
    void UpdateWorldItems(double deltaSeconds);
    void SendWorldItemSnapshots(
        const std::vector<std::pair<HSteamNetConnection, PlayerID>> &recipients, uint32_t serverTick
    );
    void SendInventorySnapshotToPlayer(PlayerID playerId);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
    std::unordered_map<uint64_t, WorldItemEntity> m_worldItems;
    uint64_t m_nextWorldItemId = 1;
};
