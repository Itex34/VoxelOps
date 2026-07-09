#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include "../../../Shared/network/Packets.hpp"

class InputBuffer {
public:
    bool enqueue(const PlayerInput &input);
    // Consume at most one unprocessed input per server simulation tick. If an
    // input tick is missing, the oldest received newer input is used; queued
    // inputs are never skipped just because a fresher packet already arrived.
    bool consumeNext(PlayerInput &outInput);
    bool peekNext(PlayerInput &outInput, uint32_t &outInputTick) const;
    void markProcessedUpTo(uint32_t processedTick);
    void reset();

    uint32_t lastProcessedInputTick() const {
        return lastProcessedInputTick_;
    }
    std::size_t pendingInputCount() const {
        return pendingInputs_.size();
    }
    bool hasReceivedInput() const {
        return hasReceivedInput_;
    }

private:
    std::map<uint32_t, PlayerInput> pendingInputs_;
    uint32_t lastProcessedInputTick_ = 0;
    bool hasReceivedInput_ = false;
};
