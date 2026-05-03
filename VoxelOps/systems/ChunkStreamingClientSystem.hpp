#pragma once

#include "../runtime/Runtime.hpp"
#include "../world/ChunkStreamingClient.hpp"

class ChunkStreamingClientSystem {
public:
    void update(Runtime &runtime, bool prioritizeMovement);

private:
    ChunkStreamingClient m_chunkStreamingClient;
};
