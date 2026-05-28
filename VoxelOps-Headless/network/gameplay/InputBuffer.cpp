#include "InputBuffer.hpp"
#include "../../../Shared/utils/Network.hpp"
#include <iostream>

namespace {
    constexpr size_t kMaxBufferedInputs = 256;
    constexpr int32_t kMaxInputLeadTicks = 120;
    constexpr int32_t kMaxInputGapTicks = 8;
} // namespace

bool InputBuffer::enqueue(const PlayerInput &input) {
    if (!hasReceivedInput_) {
        lastProcessedInputTick_ = (input.inputTick > 0) ? (input.inputTick - 1) : 0;
        hasReceivedInput_ = true;
    }

    if (!Shared::Utils::IsNewerU32(input.inputTick, lastProcessedInputTick_)) {
        return true;
    }

    const int32_t leadTicks = static_cast<int32_t>(input.inputTick - lastProcessedInputTick_);

    if (leadTicks > kMaxInputLeadTicks) {
        pendingInputs_.clear();

        lastProcessedInputTick_ = (input.inputTick > 0) ? (input.inputTick - 1) : 0;
        hasReceivedInput_ = true;

        pendingInputs_[input.inputTick] = input;

        static uint32_t s_inputRebaseLogCount = 0;
        ++s_inputRebaseLogCount;

        if (s_inputRebaseLogCount <= 30 || (s_inputRebaseLogCount % 100) == 0) {
            std::cerr << "[input] rebase inputTick=" << input.inputTick << " lead=" << leadTicks
                      << " maxLead=" << kMaxInputLeadTicks << "\n";
        }

        return true;
    }

    pendingInputs_[input.inputTick] = input;

    while (pendingInputs_.size() > kMaxBufferedInputs) {
        pendingInputs_.erase(pendingInputs_.begin());
    }

    return true;
}

bool InputBuffer::consumeNext(PlayerInput &outInput) {
    if (!hasReceivedInput_ && !pendingInputs_.empty()) {
        const uint32_t firstTick = pendingInputs_.begin()->first;
        lastProcessedInputTick_ = (firstTick > 0) ? (firstTick - 1) : 0;
        hasReceivedInput_ = true;
    }

    if (!hasReceivedInput_) {
        return false;
    }

    const uint32_t expectedTick = lastProcessedInputTick_ + 1;
    auto pendingIt = pendingInputs_.find(expectedTick);
    bool advancedInputTick = false;
    bool consumed = false;

    if (pendingIt != pendingInputs_.end()) {
        outInput = pendingIt->second;
        pendingInputs_.erase(pendingIt);
        lastProcessedInputTick_ = expectedTick;
        advancedInputTick = true;
        consumed = true;
    } else if (!pendingInputs_.empty()) {
        const uint32_t oldestTick = pendingInputs_.begin()->first;
        const uint32_t gap = (oldestTick > expectedTick) ? (oldestTick - expectedTick) : 0;
        if (gap > static_cast<uint32_t>(kMaxInputGapTicks)) {
            lastProcessedInputTick_ = (oldestTick > 0) ? (oldestTick - 1) : 0;
            advancedInputTick = true;
        }
    }

    if (advancedInputTick) {
        while (!pendingInputs_.empty() &&
               !Shared::Utils::IsNewerU32(pendingInputs_.begin()->first, lastProcessedInputTick_)) {
            pendingInputs_.erase(pendingInputs_.begin());
        }
    }

    return consumed;
}

bool InputBuffer::peekNext(PlayerInput &outInput, uint32_t &outInputTick) const {
    if (!hasReceivedInput_) {
        if (pendingInputs_.empty()) {
            return false;
        }
        const auto firstIt = pendingInputs_.begin();
        outInput = firstIt->second;
        outInputTick = firstIt->first;
        return true;
    }

    const uint32_t expectedTick = lastProcessedInputTick_ + 1;
    const auto pendingIt = pendingInputs_.find(expectedTick);
    if (pendingIt == pendingInputs_.end()) {
        return false;
    }

    outInput = pendingIt->second;
    outInputTick = pendingIt->first;
    return true;
}

void InputBuffer::markProcessedUpTo(uint32_t processedTick) {
    if (!hasReceivedInput_) {
        hasReceivedInput_ = true;
        lastProcessedInputTick_ = processedTick;
    } else if (Shared::Utils::IsNewerU32(processedTick, lastProcessedInputTick_)) {
        lastProcessedInputTick_ = processedTick;
    } else {
        return;
    }

    while (!pendingInputs_.empty() &&
           !Shared::Utils::IsNewerU32(pendingInputs_.begin()->first, lastProcessedInputTick_)) {
        pendingInputs_.erase(pendingInputs_.begin());
    }
}

void InputBuffer::reset() {
    pendingInputs_.clear();
    lastProcessedInputTick_ = 0;
    hasReceivedInput_ = false;
}
