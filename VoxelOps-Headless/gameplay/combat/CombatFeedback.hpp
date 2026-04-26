#pragma once

#include "../../player/ServerPlayer.hpp"

#include <cstdint>
#include <string>

namespace CombatFeedback {

template <typename ScoreMap>
void ApplyKillScore(ScoreMap &scores, bool matchEnded, PlayerID killerId, PlayerID victimId) {
    if (matchEnded) {
        return;
    }
    auto killerIt = scores.find(killerId);
    if (killerIt != scores.end()) {
        ++killerIt->second.kills;
    }
    auto victimIt = scores.find(victimId);
    if (victimIt != scores.end()) {
        ++victimIt->second.deaths;
    }
}

std::string FallbackVictimUsername(PlayerID victimId);

std::string BuildKillfeedPacket(const std::string &killerUsername,
                                const std::string &victimUsername,
                                uint16_t weaponId);

} // namespace CombatFeedback
