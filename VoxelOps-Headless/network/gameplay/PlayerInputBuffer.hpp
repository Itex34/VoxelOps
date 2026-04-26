#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include "../../../Shared/network/Packets.hpp"

class PlayerInputBuffer {
public:
    bool enqueue(const PlayerInput& input);
    bool consumeNext(PlayerInput& outInput);
    void reset();

    uint32_t lastProcessedInputTick() const { return lastProcessedInputTick_; }
    std::size_t pendingInputCount() const { return pendingInputs_.size(); }
    bool hasReceivedInput() const { return hasReceivedInput_; }

private:
    std::map<uint32_t, PlayerInput> pendingInputs_;
    uint32_t lastProcessedInputTick_ = 0;
    bool hasReceivedInput_ = false;
};
