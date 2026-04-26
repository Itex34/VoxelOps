#pragma once

#include "../player/ServerPlayer.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace PlayerCombat {
std::vector<ServerPlayerCombatSnapshot>
getAllCombatSnapshotsCopy(const std::unordered_map<PlayerID, ServerPlayer> &playersById,
                          bool aliveOnly);

bool applyDamage(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                 PlayerID id,
                 float damage,
                 float &outHealthAfter,
                 bool &outKilled,
                 std::chrono::milliseconds respawnDelay,
                 Clock::time_point now);
} // namespace PlayerCombat
