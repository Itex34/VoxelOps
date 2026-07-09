#include "PlayerSnapshots.hpp"

#include "../../../Shared/network/Packets.hpp"
#include "PlayerSnapshotSerializer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

using Clock = std::chrono::steady_clock;
namespace {
    constexpr float kChunkSizeMeters = 16.0f;
    constexpr float kSnapshotInterestMarginMeters = 32.0f;

    bool IsRelevantToRecipient(
        const ReplicationPlayerState &recipient,
        const ReplicationPlayerState &candidate,
        uint16_t viewDistance
    ) {
        if (recipient.id == candidate.id) {
            return true;
        }
        if (!candidate.isAlive) {
            return true;
        }

        const float radiusMeters =
            static_cast<float>(std::max<uint16_t>(viewDistance, 2)) * kChunkSizeMeters +
            kSnapshotInterestMarginMeters;
        const glm::vec3 delta = candidate.position - recipient.position;
        const float horizontalDist2 = delta.x * delta.x + delta.z * delta.z;
        return horizontalDist2 <= radiusMeters * radiusMeters;
    }

    PlayerSnapshot MakePlayerSnapshot(
        const ReplicationPlayerState &player, std::chrono::steady_clock::time_point now
    ) {
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
            const float remaining = std::chrono::duration<float>(player.respawnAt - now).count();
            pkt.respawnSeconds = std::max(0.0f, remaining);
        }
        pkt.jumpPressedLastTick = player.jumpPressedLastTick ? 1u : 0u;
        pkt.timeSinceGrounded = player.timeSinceGrounded;
        pkt.jumpBufferTimer = player.jumpBufferTimer;
        pkt.stepCooldownTimer = player.stepCooldownTimer;
        return pkt;
    }

    std::vector<uint8_t> buildPlayersPayload(
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById,
        std::chrono::steady_clock::time_point now
    ) {
        std::vector<PlayerSnapshot> players;
        players.reserve(playersById.size());

        for (const auto &entry : playersById) {
            players.push_back(MakePlayerSnapshot(entry.second, now));
        }

        return PlayerSnapshotSerializer::serializePlayers(players);
    }

    std::vector<uint8_t> buildPlayersPayloadForRecipient(
        const ReplicationPlayerState &recipient,
        uint16_t viewDistance,
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById,
        std::chrono::steady_clock::time_point now
    ) {
        std::vector<PlayerSnapshot> players;
        players.reserve(playersById.size());

        for (const auto &[_, player] : playersById) {
            if (!IsRelevantToRecipient(recipient, player, viewDistance)) {
                continue;
            }
            players.push_back(MakePlayerSnapshot(player, now));
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

    std::vector<std::vector<uint8_t>> buildSnapshotsForRecipients(
        const std::vector<std::pair<PlayerID, uint16_t>> &recipients,
        uint32_t serverTick,
        const std::unordered_map<PlayerID, ReplicationPlayerState> &playersById
    ) {
        std::vector<std::vector<uint8_t>> frames;
        frames.reserve(recipients.size());
        if (recipients.empty()) {
            return frames;
        }

        const auto now = Clock::now();
        for (const auto &[recipientId, viewDistance] : recipients) {
            const auto recipientIt = playersById.find(recipientId);
            if (recipientIt == playersById.end()) {
                frames.emplace_back();
                continue;
            }

            const std::vector<uint8_t> playersPayload = buildPlayersPayloadForRecipient(
                recipientIt->second, viewDistance, playersById, now
            );
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
