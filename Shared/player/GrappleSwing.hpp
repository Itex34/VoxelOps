#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace Shared::Grapple {

inline constexpr float kMinRopeLength = 1.25f;
inline constexpr float kReelInSpeedUnitsPerSecond = 14.0f;

inline void ApplyReelIn(
    float &ropeLength,
    float dt,
    float speed = kReelInSpeedUnitsPerSecond,
    float minRopeLength = kMinRopeLength
) {
    if (!std::isfinite(ropeLength) || !std::isfinite(dt) || !std::isfinite(speed)) {
        return;
    }
    if (ropeLength <= minRopeLength || dt <= 0.0f || speed <= 0.0f) {
        ropeLength = std::max(ropeLength, minRopeLength);
        return;
    }
    ropeLength = std::max(minRopeLength, ropeLength - (speed * dt));
}

template <typename CollisionFn>
inline void ApplyRopeConstraint(
    const glm::vec3 &anchor,
    float ropeLength,
    glm::vec3 &position,
    glm::vec3 &velocity,
    CollisionFn &&collides
) {
    if (!std::isfinite(ropeLength) || ropeLength <= 0.0f) {
        return;
    }

    const glm::vec3 toAnchor = anchor - position;
    const float dist = glm::length(toAnchor);
    if (!std::isfinite(dist) || dist <= 1e-5f || dist <= ropeLength) {
        return;
    }

    const glm::vec3 dir = toAnchor / dist;
    const float radialAlongAnchor = glm::dot(velocity, dir);
    if (radialAlongAnchor < 0.0f) {
        velocity -= dir * radialAlongAnchor;
    }

    const glm::vec3 targetPos = anchor - (dir * ropeLength);
    if (!collides(targetPos)) {
        position = targetPos;
        return;
    }

    if (collides(position)) {
        return;
    }

    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 10; ++i) {
        const float mid = 0.5f * (lo + hi);
        const glm::vec3 testPos = glm::mix(position, targetPos, mid);
        if (collides(testPos)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    if (lo > 1e-4f) {
        position = glm::mix(position, targetPos, lo);
    }
}

} // namespace Shared::Grapple
