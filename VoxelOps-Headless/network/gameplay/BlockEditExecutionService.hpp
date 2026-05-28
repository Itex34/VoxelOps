#pragma once

#include "../../../Shared/network/Packets.hpp"
#include "../../../Shared/player/PlayerID.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class BlockEditExecutionService {
public:
    using ChunkCoord = ClientSessionManager::ChunkCoord;
    using ChunkCoordHash = ClientSessionManager::ChunkCoordHash;

    BlockEditExecutionService(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        ChunkManager &chunkManager
    );

    BlockPlaceResult ExecuteBlockPlaceRequest(PlayerID requesterId, const BlockPlaceRequest &request);
    BlockBreakResult ExecuteBlockBreakRequest(PlayerID requesterId, const BlockBreakRequest &request);

private:
    bool IsAnyAlivePlayerOccupyingBlock(
        const std::vector<ServerPlayer> &players, const glm::ivec3 &worldPos
    ) const;
    bool ApplyBlockEditsAndBuildDeltas(
        const std::unordered_map<glm::ivec3, BlockID, IVec3Hash, IVec3Eq> &normalizedEdits,
        std::vector<ChunkDelta> &outboundDeltas
    );
    void BroadcastChunkDeltas(const std::vector<ChunkDelta> &outboundDeltas);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
};
