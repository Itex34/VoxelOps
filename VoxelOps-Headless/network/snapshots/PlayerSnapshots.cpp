#include "PlayerSnapshots.hpp"

#include "../../../Shared/network/Packets.hpp"
#include "PlayerSnapshotSerializer.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

using Clock = std::chrono::steady_clock;
namespace {
    std::vector<uint8_t> buildPlayersPayload(
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById,
        std::chrono::steady_clock::time_point now
    ) {
        std::vector<PlayerSnapshot> players;
        players.reserve(playersById.size());

        for (const auto &entry : playersById) {
            const ReplicationPlayerState &player = entry.second;
            PlayerSnapshot pkt{};
            pkt.id = player.id;
            pkt.px = player.position.x;
            pkt.py = player.position.y;
            pkt.pz = player.position.z;
            pkt.vx = player.velocity.x;
            pkt.vy = player.velocity.y;
            pkt.vz = player.velocity.z;
            pkt.yaw = player.yaw;
            pkt.pitch = player.pitch;
            pkt.onGround = player.onGround ? 1 : 0;
            pkt.flyMode = player.flyMode ? 1 : 0;
            pkt.allowFlyMode = player.allowFlyMode ? 1 : 0;
            pkt.weaponId = player.weaponId;
            pkt.health = player.health;
            pkt.isAlive = player.isAlive ? 1 : 0;
            if (player.isAlive || player.respawnAt == std::chrono::steady_clock::time_point{}) {
                pkt.respawnSeconds = 0.0f;
            } else {
                const float remaining =
                    std::chrono::duration<float>(player.respawnAt - now).count();
                pkt.respawnSeconds = std::max(0.0f, remaining);
            }
            pkt.jumpPressedLastTick = player.jumpPressedLastTick ? 1u : 0u;
            pkt.timeSinceGrounded = player.timeSinceGrounded;
            pkt.jumpBufferTimer = player.jumpBufferTimer;
            players.push_back(pkt);
        }

        return PlayerSnapshotSerializer::serializePlayers(players);
    }
} // namespace

namespace PlayerSnapshots {

    std::vector<uint8_t> buildSnapshotFor(
        PlayerID recipientId,
        uint32_t serverTick,
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById
    ) {
        const std::vector<PlayerID> recipients{recipientId};
        std::vector<std::vector<uint8_t>> snapshots =
            buildSnapshotsForRecipients(recipients, serverTick, playersById);
        if (snapshots.empty()) {
            return {};
        }
        return std::move(snapshots.front());
    }

    std::vector<std::vector<uint8_t>> buildSnapshotsForRecipients(
        const std::vector<PlayerID> &recipientIds,
        uint32_t serverTick,
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById
    ) {
        std::vector<std::vector<uint8_t>> frames;
        frames.reserve(recipientIds.size());
        if (recipientIds.empty()) {
            return frames;
        }

        const std::vector<uint8_t> playersPayload = buildPlayersPayload(playersById, Clock::now());
        for (const PlayerID recipientId : recipientIds) {
            const auto recipientIt = playersById.find(recipientId);
            if (recipientIt == playersById.end()) {
                frames.emplace_back();
                continue;
            }

            frames.push_back(
                PlayerSnapshotSerializer::buildFrame(
                    serverTick,
                    recipientId,
                    recipientIt->second.lastProcessedInputTick,
                    playersPayload
                )
            );
        }

        return frames;
    }

} // namespace PlayerSnapshots
