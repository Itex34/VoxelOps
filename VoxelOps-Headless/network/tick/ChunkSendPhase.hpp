#pragma once

#include <chrono>
#include <cstddef>
#include <functional>

class ChunkSendPhase {
public:
    struct Hooks {
        std::function<size_t(size_t, size_t)> flushChunkSendQueues;
    };

    explicit ChunkSendPhase(Hooks hooks);

    size_t RunChunkSendPhase(
        bool simBacklog,
        std::chrono::steady_clock::time_point &nextChunkSendFlushAt,
        double &chunkSendUs
    );

private:
    Hooks m_hooks;
};
