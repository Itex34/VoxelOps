#pragma once

#include "../../player/ServerPlayer.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace PlayerSnapshotSystem {
std::vector<uint8_t>
buildSnapshotFor(PlayerID recipientId,
                 uint32_t serverTick,
                 const std::unordered_map<PlayerID, ServerPlayer> &playersById);

std::vector<std::vector<uint8_t>>
buildSnapshotsForRecipients(const std::vector<PlayerID> &recipientIds,
                            uint32_t serverTick,
                            const std::unordered_map<PlayerID, ServerPlayer> &playersById);
} // namespace PlayerSnapshotSystem
