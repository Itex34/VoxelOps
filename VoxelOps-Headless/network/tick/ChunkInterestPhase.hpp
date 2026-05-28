#pragma once

#include "../session/ClientSessionManager.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <glm/ext/vector_int3.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class ChunkInterestPhase {
public:
    struct Hooks {
        std::function<void(HSteamNetConnection, const glm::ivec3 &, uint16_t)>
            updateChunkStreamingForClient;
    };

    ChunkInterestPhase(std::mutex &mutex, ClientSessionManager &sessions, Hooks hooks);

    size_t RunChunkInterestPhase(bool simBacklog, double &chunkInterestUs);

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    Hooks m_hooks;
};
