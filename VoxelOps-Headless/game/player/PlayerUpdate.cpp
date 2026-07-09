#include "PlayerUpdate.hpp"

#include "../../../Shared/utils/Math.hpp"

#include "../spawn/Respawning.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "ServerMovementSimulation.hpp"

#include <cmath>
#include <iostream>

namespace {
    void clearDeadPlayerMotionState(ServerPlayer &player) {
        player.activeInputFlags = 0;
        player.moveX = 0.0f;
        player.moveZ = 0.0f;
        player.flyMode = false;
        player.velocity = glm::vec3(0.0f);
        player.onGround = false;
        player.jumpPressedLastTick = false;
        player.timeSinceGrounded = 0.0f;
        player.jumpBufferTimer = 0.0f;
        player.stepCooldownTimer = 0.0f;
    }
} // namespace

namespace PlayerUpdate {

    void handleRespawn(
        ServerPlayer &player,
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        ChunkManager &chunkManager,
        Clock::time_point now
    ) {
        PlayerInput cmd{};
        if (player.inputBuffer.consumeNext(cmd)) {
            player.yaw =
                Shared::Utils::NormalizeYawDegrees(std::isfinite(cmd.yaw) ? cmd.yaw : 0.0f);
            player.pitch = std::isfinite(cmd.pitch) ? cmd.pitch : 0.0f;
        }

        clearDeadPlayerMotionState(player);

        if (Respawning::TryRespawnPlayer(player, playersById, chunkManager, now)) {
            player.lastInputReceived = now;
        }
    }

    void handleTimeouts(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        std::list<PlayerID> &playersOrder,
        const std::vector<PlayerID> &playerIdsToRemove
    ) {
        for (const PlayerID id : playerIdsToRemove) {
            const auto it = playersById.find(id);
            if (it == playersById.end()) {
                continue;
            }

            playersOrder.erase(it->second.orderIt);
            playersById.erase(it);
            std::cout << "Player " << id << " timed out and removed\n";
        }
    }

    void updatePlayers(
        std::unordered_map<PlayerID, ServerPlayer> &playersById,
        std::list<PlayerID> &playersOrder,
        double deltaSeconds,
        ChunkManager &chunkManager,
        std::chrono::seconds heartbeatTimeout
    ) {
        const auto now = Clock::now();
        std::vector<PlayerID> timedOutPlayerIds;
        timedOutPlayerIds.reserve(playersById.size());

        for (auto &entry : playersById) {
            ServerPlayer &player = entry.second;
            if (player.isAlive) {
                ServerMovementSimulation::simulatePhysicsForPlayer(
                    player, deltaSeconds, chunkManager
                );
            } else {
                handleRespawn(player, playersById, chunkManager, now);
            }

            if (now - player.lastHeartbeat > heartbeatTimeout) {
                timedOutPlayerIds.push_back(entry.first);
            }
        }

        handleTimeouts(playersById, playersOrder, timedOutPlayerIds);
    }

} // namespace PlayerUpdate
