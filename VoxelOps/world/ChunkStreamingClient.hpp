#pragma once

#include "../runtime/Runtime.hpp"

#include <unordered_map>

class ChunkStreamingClient {
public:
    void update(Runtime &runtime, bool prioritizeMovement);

private:
    double m_nextChunkResyncSendAt = 0.0;
    std::unordered_map<glm::ivec3, double, IVec3Hash> m_chunkResyncCooldownUntil;
};
