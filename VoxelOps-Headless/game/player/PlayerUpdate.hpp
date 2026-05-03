#pragma once

#include "ServerPlayer.hpp"

#include <chrono>
#include <list>
#include <unordered_map>
#include <vector>

class ChunkManager;

namespace PlayerUpdate {
    void handleRespawn(
        ServerPlayer &player,
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        ChunkManager &chunkManager,
        Clock::time_point now
    );

    void handleTimeouts(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        std::list<PlayerID> &playersOrder,
        const std::vector<PlayerID> &playerIdsToRemove
    );

    void updatePlayers(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        std::list<PlayerID> &playersOrder,
        double deltaSeconds,
        ChunkManager &chunkManager,
        std::chrono::seconds heartbeatTimeout
    );
} // namespace PlayerUpdate
