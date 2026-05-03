#include "CombatFeedback.hpp"

#include "../../../Shared/network/PacketType.hpp"

namespace CombatFeedback {

    std::string FallbackVictimUsername(PlayerID victimId) {
        return std::string("Player") + std::to_string(victimId);
    }

    std::string BuildKillfeedPacket(
        const std::string &killerUsername, const std::string &victimUsername, uint16_t weaponId
    ) {
        std::string killPayload = "KILLFEED|";
        killPayload += killerUsername;
        killPayload += "|";
        killPayload += victimUsername;
        killPayload += "|";
        killPayload += std::to_string(weaponId);

        std::string out;
        out.reserve(1 + killPayload.size());
        out.push_back(static_cast<char>(PacketType::Message));
        out += killPayload;
        return out;
    }

} // namespace CombatFeedback
