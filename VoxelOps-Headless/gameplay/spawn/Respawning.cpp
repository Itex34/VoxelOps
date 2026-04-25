#include "Respawning.hpp"

#include "../../world/ChunkManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <glm/glm.hpp>

namespace Respawning
{
namespace {
constexpr int kRespawnCollisionPrewarmRadiusXZ = 2;
constexpr int kRespawnCollisionPrewarmRadiusY = 1;
const std::array<glm::vec3, 9> kRespawnCandidates{ {
    glm::vec3(0.0f, 60.0f, 0.0f),
    glm::vec3(14.0f, 60.0f, 14.0f),
    glm::vec3(-14.0f, 60.0f, 14.0f),
    glm::vec3(14.0f, 60.0f, -14.0f),
    glm::vec3(-14.0f, 60.0f, -14.0f),
    glm::vec3(24.0f, 60.0f, 0.0f),
    glm::vec3(-24.0f, 60.0f, 0.0f),
    glm::vec3(0.0f, 60.0f, 24.0f),
    glm::vec3(0.0f, 60.0f, -24.0f)
} };

glm::vec3 ChooseRespawnPosition(
    const std::unordered_map<PlayerID, ServerPlayer>& playersById,
    PlayerID respawningId
) {
    if (kRespawnCandidates.empty()) {
        return glm::vec3(0.0f, 60.0f, 0.0f);
    }

    float bestScore = -1.0f;
    glm::vec3 bestPosition = kRespawnCandidates.front();
    bool foundAnyAliveOpponent = false;

    for (const glm::vec3& candidate : kRespawnCandidates) {
        float nearestAliveDistSq = std::numeric_limits<float>::max();
        bool hasAliveOpponent = false;
        for (const auto& kv : playersById) {
            if (kv.first == respawningId) {
                continue;
            }
            const ServerPlayer& other = kv.second;
            if (!other.isAlive) {
                continue;
            }
            const glm::vec3 delta = candidate - other.position;
            const float distSq = glm::dot(delta, delta);
            nearestAliveDistSq = std::min(nearestAliveDistSq, distSq);
            hasAliveOpponent = true;
        }

        if (!hasAliveOpponent) {
            if (!foundAnyAliveOpponent) {
                return candidate;
            }
            continue;
        }

        foundAnyAliveOpponent = true;
        if (nearestAliveDistSq > bestScore) {
            bestScore = nearestAliveDistSq;
            bestPosition = candidate;
        }
    }

    return bestPosition;
}

void PrewarmRespawnCollisionChunks(ChunkManager& chunkManager, const glm::vec3& respawnPos)
{
    const glm::ivec3 respawnWorldPos(
        static_cast<int>(std::floor(respawnPos.x)),
        static_cast<int>(std::floor(respawnPos.y)),
        static_cast<int>(std::floor(respawnPos.z))
    );
    const glm::ivec3 centerChunk = chunkManager.worldToChunkPos(respawnWorldPos);

    for (int dx = -kRespawnCollisionPrewarmRadiusXZ; dx <= kRespawnCollisionPrewarmRadiusXZ; ++dx) {
        for (int dz = -kRespawnCollisionPrewarmRadiusXZ; dz <= kRespawnCollisionPrewarmRadiusXZ; ++dz) {
            for (int dy = -kRespawnCollisionPrewarmRadiusY; dy <= kRespawnCollisionPrewarmRadiusY; ++dy) {
                const glm::ivec3 chunkPos(
                    centerChunk.x + dx,
                    centerChunk.y + dy,
                    centerChunk.z + dz
                );
                if (!chunkManager.inBounds(chunkPos) || chunkManager.hasChunkLoaded(chunkPos)) {
                    continue;
                }
                chunkManager.generateTerrainChunkAt(chunkPos);
            }
        }
    }
}

void RespawnPlayer(ServerPlayer& player, const glm::vec3& position) {
    player.position = position;
    player.velocity = glm::vec3(0.0f);
    player.onGround = false;
    player.flyMode = false;
    player.activeInputFlags = 0;
    player.moveX = 0.0f;
    player.moveZ = 0.0f;
    player.jumpPressedLastTick = false;
    player.timeSinceGrounded = 0.0f;
    player.jumpBufferTimer = 0.0f;
    player.inputBuffer.reset();
    player.health = player.maxHealth;
    player.isAlive = true;
    player.respawnAt = Clock::time_point{};
    player.pendingRespawnRequest = false;
}
}

void MarkPlayerDead(
    ServerPlayer& player,
    Clock::time_point now,
    std::chrono::milliseconds respawnDelay
) {
    player.health = 0.0f;
    player.isAlive = false;
    player.respawnAt = now + respawnDelay;
    player.pendingRespawnRequest = false;
    player.velocity = glm::vec3(0.0f);
    player.flyMode = false;
    player.activeInputFlags = 0;
    player.moveX = 0.0f;
    player.moveZ = 0.0f;
    player.onGround = false;
    player.jumpPressedLastTick = false;
    player.timeSinceGrounded = 0.0f;
    player.jumpBufferTimer = 0.0f;
    player.inputBuffer.reset();
}

bool RequestRespawn(ServerPlayer& player, Clock::time_point now) {
    if (player.isAlive) {
        return false;
    }

    if (player.respawnAt == Clock::time_point{} || now < player.respawnAt) {
        return false;
    }

    player.pendingRespawnRequest = true;
    return true;
}

bool TryRespawnPlayer(
    ServerPlayer& player,
    const std::unordered_map<PlayerID, ServerPlayer>& playersById,
    ChunkManager& chunkManager,
    Clock::time_point now
) {
    if (
        !player.pendingRespawnRequest ||
        player.respawnAt == Clock::time_point{} ||
        now < player.respawnAt
    ) {
        return false;
    }

    const glm::vec3 respawnPos = ChooseRespawnPosition(playersById, player.id);
    PrewarmRespawnCollisionChunks(chunkManager, respawnPos);
    RespawnPlayer(player, respawnPos);
    return true;
}

}
