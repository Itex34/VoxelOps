#include "ChunkStreamingClientSystem.hpp"

void ChunkStreamingClientSystem::update(Runtime &runtime, bool prioritizeMovement) {
    m_chunkStreamingClient.update(runtime, prioritizeMovement);
}
