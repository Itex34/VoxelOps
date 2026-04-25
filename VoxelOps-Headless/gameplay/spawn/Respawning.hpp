#pragma once

#include "../../player/ServerPlayer.hpp"

#include <chrono>
#include <unordered_map>

class ChunkManager;

namespace Respawning
{
    void MarkPlayerDead(
        ServerPlayer& player,
        Clock::time_point now,
        std::chrono::milliseconds respawnDelay
    );

    bool RequestRespawn(ServerPlayer& player, Clock::time_point now);

    bool TryRespawnPlayer(
        ServerPlayer& player,
        const std::unordered_map<PlayerID, ServerPlayer>& playersById,
        ChunkManager& chunkManager,
        Clock::time_point now
    );

}
