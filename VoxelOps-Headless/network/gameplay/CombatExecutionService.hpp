#pragma once

#include "../../../Shared/network/Packets.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../game/combat/LagCompensation.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

class CombatExecutionService {
public:
    struct Hooks {
        std::function<void(PlayerID, std::string_view, PlayerID, uint16_t)> onConfirmedKill;
    };

    CombatExecutionService(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        ChunkManager &chunkManager,
        std::atomic<uint32_t> &serverTick,
        Hooks hooks
    );

    void InvalidateCombatSnapshotCache();
    void RecordLagCompFrame(uint32_t serverTick);
    ShootResult ExecuteShootRequest(HSteamNetConnection incoming, const ShootRequest &req);
    GrappleResult ExecuteGrappleRequest(HSteamNetConnection incoming, const GrappleRequest &req);

private:
    const std::vector<ServerPlayerCombatSnapshot> &GetCombatSnapshotsForTick(uint32_t serverTick);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
    std::atomic<uint32_t> &m_serverTick;
    Hooks m_hooks;
    std::deque<LagCompensation::LagCompFrame> m_lagCompFrames;
    std::vector<ServerPlayerCombatSnapshot> m_combatSnapshotsAliveCache;
    uint32_t m_combatSnapshotsAliveCacheTick = 0;
    bool m_hasCombatSnapshotsAliveCache = false;
};
