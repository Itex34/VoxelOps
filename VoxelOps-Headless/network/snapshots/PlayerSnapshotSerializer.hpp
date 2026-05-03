#pragma once
#include <cstdint>
#include <vector>

struct PlayerSnapshot;

namespace PlayerSnapshotSerializer {
    std::vector<uint8_t> serializePlayers(const std::vector<PlayerSnapshot> &players);
    std::vector<uint8_t> buildFrame(
        uint32_t serverTick,
        uint64_t selfPlayerId,
        uint32_t lastProcessedInputTick,
        const std::vector<uint8_t> &playersPayload
    );
} // namespace PlayerSnapshotSerializer
