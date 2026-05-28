#pragma once

#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "../session/ClientSessionManager.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>

class CollisionPrewarmPhase {
public:
    CollisionPrewarmPhase(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        ChunkManager &chunkManager
    );

    size_t RunCollisionPrewarmPhase(
        bool simBacklog,
        std::chrono::steady_clock::time_point &nextCollisionPrewarmAt,
        double &collisionPrewarmUs
    );

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
};
