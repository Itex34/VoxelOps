#include "WorldItemPhysics.hpp"

#include "../graphics/ChunkManager.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMaxAxisStepDistance = 0.24f;
constexpr float kGroundProbeDistance = 0.04f;
constexpr float kMaxNudgeUp = 0.40f;
constexpr float kMinHorizontalSleepSpeed = 0.01f;
}

void WorldItemPhysicsSystem::Step(
    WorldItemEntity& item,
    float deltaSeconds,
    float tickRateHz,
    const ChunkManager& chunkManager
)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
        return;
    }

    const float safeTickRateHz = std::max(1.0f, tickRateHz);
    item.velocity.y -= kGravity * deltaSeconds;

    const float speedSq = glm::dot(item.velocity, item.velocity);
    const float maxSpeedSq = kMaxSpeed * kMaxSpeed;
    if (speedSq > maxSpeedSq && speedSq > 1e-6f) {
        item.velocity *= (kMaxSpeed / std::sqrt(speedSq));
    }

    glm::vec3 position = item.position;
    auto overlap = chunkManager.queryAabbCollision(position, kCollisionRadius, kCollisionHeight, true);
    if (overlap.collided) {
        const int maxNudgeSteps = static_cast<int>(std::ceil(kMaxNudgeUp / kGroundProbeDistance));
        for (int i = 0; i < maxNudgeSteps; ++i) {
            position.y += kGroundProbeDistance;
            overlap = chunkManager.queryAabbCollision(position, kCollisionRadius, kCollisionHeight, true);
            if (!overlap.collided) {
                break;
            }
        }
        if (overlap.collided) {
            item.velocity = glm::vec3(0.0f);
            item.position = position;
            return;
        }
    }

    bool onGround = false;
    const glm::vec3 delta = item.velocity * deltaSeconds;
    for (int axis = 0; axis < 3; ++axis) {
        const float move = delta[axis];
        if (!std::isfinite(move) || std::abs(move) < 1e-6f) {
            continue;
        }

        const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(move) / kMaxAxisStepDistance)));
        const float stepMove = move / static_cast<float>(steps);
        for (int i = 0; i < steps; ++i) {
            glm::vec3 candidate = position;
            candidate[axis] += stepMove;
            const auto query = chunkManager.queryAabbCollision(candidate, kCollisionRadius, kCollisionHeight, true);
            if (!query.collided) {
                position = candidate;
                continue;
            }

            if (axis == 1 && stepMove < 0.0f) {
                onGround = true;
            }
            item.velocity[axis] = 0.0f;
            break;
        }
    }

    if (!onGround) {
        glm::vec3 probe = position;
        probe.y -= kGroundProbeDistance;
        const auto query = chunkManager.queryAabbCollision(probe, kCollisionRadius, kCollisionHeight, true);
        onGround = query.collided;
    }

    const float dampingPerTick = onGround ? kGroundDampingPerTick : kAirDampingPerTick;
    const float damping = std::pow(dampingPerTick, deltaSeconds * safeTickRateHz);
    item.velocity.x *= damping;
    item.velocity.z *= damping;

    if (onGround && item.velocity.y < 0.0f) {
        item.velocity.y = 0.0f;
    }
    if (std::abs(item.velocity.x) < kMinHorizontalSleepSpeed) {
        item.velocity.x = 0.0f;
    }
    if (std::abs(item.velocity.z) < kMinHorizontalSleepSpeed) {
        item.velocity.z = 0.0f;
    }

    item.position = position;
}
