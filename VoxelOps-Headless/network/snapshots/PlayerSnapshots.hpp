#pragma once

#include "ReplicationPlayerState.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace PlayerSnapshots {
    std::vector<uint8_t> buildSnapshotFor(
        PlayerID recipientId,
        uint32_t serverTick,
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById
    );

    std::vector<std::vector<uint8_t>> buildSnapshotsForRecipients(
        const std::vector<PlayerID> &recipientIds,
        uint32_t serverTick,
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById
    );
} // namespace PlayerSnapshots
