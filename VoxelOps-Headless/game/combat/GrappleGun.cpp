#include "GrappleGun.hpp"
#include "../../engine/world/WorldRaycast.hpp"
#include "../../engine/world/ChunkManager.hpp"

#include <algorithm>
#include <cmath>

GrappleFireResult GrappleGun::tryFire(const GrappleContext &ctx) {
    GrappleFireResult result{};
    if (ctx.nowSeconds < ctx.state.nextAllowedFireTime) {
        return result;
    }

    ctx.state.nextAllowedFireTime = ctx.nowSeconds + static_cast<double>(m_coolDownSeconds);
    result.accepted = true;

    const WorldRaycastResult raycastResult = WorldRaycast::FindFirstSolidBlockHit(
        ctx.chunkManager,
        ctx.origin,
        ctx.direction,
        m_maxGrappleDistance
    );

    if (!raycastResult.hit) {
        ctx.state.active = false;
        ctx.state.ropeLength = 0.0f;
        ctx.state.reelingIn = false;
        return result;
    }

    ctx.state.active = true;
    ctx.state.anchor = raycastResult.hitPoint;
    const glm::vec3 toAnchor = raycastResult.hitPoint - ctx.playerPosition;
    const float dist = glm::length(toAnchor);
    ctx.state.ropeLength = std::max(0.0f, dist);
    ctx.state.reelingIn = false;

    result.attached = true;
    result.anchor = raycastResult.hitPoint;
    result.face = raycastResult.face;
    result.blockNormal = static_cast<uint8_t>(raycastResult.face);
    return result;
}

void GrappleGun::release(GrappleState &state) {
    state.active = false;
    state.ropeLength = 0.0f;
    state.reelingIn = false;
}

void GrappleGun::updatePull(
    GrappleState &state, glm::vec3 &playerPos, glm::vec3 &playerVel, float dt
) {
    if (!state.active || !std::isfinite(dt) || dt <= 0.0f) {
        return;
    }

    const glm::vec3 toAnchor = state.anchor - playerPos;
    const float dist = glm::length(toAnchor);
    if (!std::isfinite(dist) || dist <= 1e-5f) {
        return;
    }

    if (state.ropeLength <= 0.0f) {
        state.ropeLength = dist;
    }
    if (dist <= state.ropeLength) {
        return;
    }

    const glm::vec3 dir = toAnchor / dist;

    // Rope acts as a pivot constraint: keep player on/inside rope sphere without pulling inward.
    playerPos = state.anchor - (dir * state.ropeLength);

    // Remove outward radial velocity when rope is taut; keep tangential component for swinging.
    const float radialAlongAnchor = glm::dot(playerVel, dir);
    if (radialAlongAnchor < 0.0f) {
        playerVel -= dir * radialAlongAnchor;
    }
}
