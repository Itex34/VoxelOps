#include "PlayerSnapshotSerializer.hpp"
#include "../../../Shared/network/PacketType.hpp"
#include "../../../Shared/network/Packets.hpp"
#include "../../../Shared/utils/Network.hpp"

namespace PlayerSnapshotSerializer {
    namespace {
        constexpr std::size_t kPlayerSnapshotEntrySize =
            8 + (8 * 4) + 3 + 2 + 4 + 1 + 4 + 1 + 4 + 4 + 4;
    } // namespace

    std::vector<uint8_t> serializePlayers(const std::vector<PlayerSnapshot> &players) {
        std::vector<uint8_t> out;
        out.reserve(4 + players.size() * kPlayerSnapshotEntrySize);
        Shared::Utils::appendU32(out, static_cast<uint32_t>(players.size()));
        for (const PlayerSnapshot &p : players) {
            Shared::Utils::appendU64(out, p.id);
            Shared::Utils::appendF32(out, p.px);
            Shared::Utils::appendF32(out, p.py);
            Shared::Utils::appendF32(out, p.pz);
            Shared::Utils::appendF32(out, p.vx);
            Shared::Utils::appendF32(out, p.vy);
            Shared::Utils::appendF32(out, p.vz);
            Shared::Utils::appendF32(out, p.yaw);
            Shared::Utils::appendF32(out, p.pitch);
            Shared::Utils::appendU8(out, p.onGround);
            Shared::Utils::appendU8(out, p.flyMode);
            Shared::Utils::appendU8(out, p.allowFlyMode);
            Shared::Utils::appendU16(out, p.weaponId);
            Shared::Utils::appendF32(out, p.health);
            Shared::Utils::appendU8(out, p.isAlive);
            Shared::Utils::appendF32(out, p.respawnSeconds);
            Shared::Utils::appendU8(out, p.jumpPressedLastTick ? 1u : 0u);
            Shared::Utils::appendF32(out, p.timeSinceGrounded);
            Shared::Utils::appendF32(out, p.jumpBufferTimer);
            Shared::Utils::appendF32(out, p.stepCooldownTimer);
        }
        return out;
    }

    std::vector<uint8_t> buildFrame(
        uint32_t serverTick,
        uint64_t selfPlayerId,
        uint32_t lastProcessedInputTick,
        const std::vector<uint8_t> &playersPayload
    ) {
        std::vector<uint8_t> out;
        out.reserve(1 + 4 + 8 + 4 + playersPayload.size());
        Shared::Utils::appendU8(out, static_cast<uint8_t>(PacketType::PlayerSnapshot));
        Shared::Utils::appendU32(out, serverTick);
        Shared::Utils::appendU64(out, selfPlayerId);
        Shared::Utils::appendU32(out, lastProcessedInputTick);
        out.insert(out.end(), playersPayload.begin(), playersPayload.end());
        return out;
    }
} // namespace PlayerSnapshotSerializer
