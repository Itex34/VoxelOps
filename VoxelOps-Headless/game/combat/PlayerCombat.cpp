#include "PlayerCombat.hpp"

#include "../spawn/Respawning.hpp"

#include <algorithm>
#include <cmath>

namespace PlayerCombat {

    std::vector<ServerPlayerCombatSnapshot> getAllCombatSnapshotsCopy(
        const std::unordered_map<PlayerID, ServerPlayer> &playersById, bool aliveOnly
    ) {
        std::vector<ServerPlayerCombatSnapshot> players;
        players.reserve(playersById.size());
        for (const auto &entry : playersById) {
            const ServerPlayer &src = entry.second;
            if (aliveOnly && !src.isAlive) {
                continue;
            }

            ServerPlayerCombatSnapshot snapshot;
            snapshot.id = src.id;
            snapshot.position = src.position;
            snapshot.yaw = src.yaw;
            snapshot.height = src.height;
            snapshot.radius = src.radius;
            snapshot.isAlive = src.isAlive;
            players.push_back(snapshot);
        }
        return players;
    }

    bool applyDamage(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        PlayerID id,
        float damage,
        float &outHealthAfter,
        bool &outKilled,
        std::chrono::milliseconds respawnDelay,
        Clock::time_point now
    ) {
        outHealthAfter = 0.0f;
        outKilled = false;
        if (!std::isfinite(damage) || damage <= 0.0f) {
            return false;
        }

        const auto it = playersById.find(id);
        if (it == playersById.end()) {
            return false;
        }

        ServerPlayer &target = it->second;
        if (!target.isAlive) {
            outHealthAfter = 0.0f;
            outKilled = false;
            return false;
        }

        target.health = std::max(0.0f, target.health - damage);
        outHealthAfter = target.health;
        if (target.health <= 0.0f) {
            outKilled = true;
            Respawning::MarkPlayerDead(target, now, respawnDelay);
            outHealthAfter = 0.0f;
        }
        return true;
    }

} // namespace PlayerCombat
