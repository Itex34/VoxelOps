#include "ChunkSendPhase.hpp"

#include <chrono>
#include <utility>

ChunkSendPhase::ChunkSendPhase(Hooks hooks)
    : m_hooks(std::move(hooks)) {}

size_t ChunkSendPhase::RunChunkSendPhase(
    bool simBacklog,
    std::chrono::steady_clock::time_point &nextChunkSendFlushAt,
    double &chunkSendUs
) {
    constexpr size_t kChunkSendGlobalBudgetPerFlush = 128;
    constexpr size_t kChunkSendPerClientBudgetPerFlush = 16;
    constexpr size_t kBacklogChunkSendGlobalBudgetPerFlush = 48;
    constexpr size_t kBacklogChunkSendPerClientBudgetPerFlush = 6;
    const auto kChunkSendFlushInterval = std::chrono::milliseconds(16);

    chunkSendUs = 0.0;

    const auto chunkSendNow = std::chrono::steady_clock::now();
    if (chunkSendNow < nextChunkSendFlushAt) {
        return 0;
    }

    const auto chunkSendStart = std::chrono::steady_clock::now();
    const size_t globalBudget =
        simBacklog ? kBacklogChunkSendGlobalBudgetPerFlush : kChunkSendGlobalBudgetPerFlush;
    const size_t perClientBudget = simBacklog ? kBacklogChunkSendPerClientBudgetPerFlush
                                              : kChunkSendPerClientBudgetPerFlush;
    const size_t chunksSentThisLoop = m_hooks.flushChunkSendQueues(
        globalBudget, perClientBudget
    );
    chunkSendUs = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - chunkSendStart
    )
                                          .count());
    nextChunkSendFlushAt = chunkSendNow + kChunkSendFlushInterval;
    return chunksSentThisLoop;
}
