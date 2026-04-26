#include "Shoot.hpp"

#include "../../world/ChunkManager.hpp"
#include "ShootValidation.hpp"

#include <cstdint>
#include <limits>

namespace Shoot {

ShootContext BuildContext(HSteamNetConnection connection,
                          const ShootRequest &request,
                          uint32_t currentServerTick) {
    ShootContext ctx{};
    ctx.connection = connection;
    ctx.request = request;
    ctx.serverTick = currentServerTick;

    ctx.result.clientShotId = request.clientShotId;
    ctx.result.serverTick = currentServerTick;
    ctx.result.accepted = 0;
    ctx.result.didHit = 0;
    ctx.result.hitEntityId = -1;
    ctx.result.serverSeed = request.seed;
    ctx.result.newAmmoCount = 0;
    return ctx;
}

ShootOutcome ResolveHit(const ShootContext &ctx,
                        const HitDetection::HitDetectionResult &hit,
                        float blockOcclusionEpsilon,
                        PlayerManager &playerManager) {
    ShootOutcome outcome{};
    outcome.accepted = true;

    if (!hit.playerHit ||
        (hit.blockHit && (hit.blockDistance + blockOcclusionEpsilon) <= hit.bestPlayerDistance)) {
        outcome.hit = false;
        outcome.hitPoint = hit.blockHit ? hit.blockHitPoint : (ctx.rayOrigin + ctx.rayDir * hit.maxDistance);
        return outcome;
    }

    outcome.hitPlayerId = hit.hitPlayerId;
    outcome.hitRegion = hit.hitRegion;
    outcome.hitPoint = hit.hitPoint;

    const Damage::DamageResolution damage =
        Damage::ResolveDamage(playerManager, hit.hitPlayerId, ctx.request.weaponId, hit.hitRegion);
    if (!damage.applied) {
        outcome.hit = false;
        outcome.hitPoint = hit.hitPoint;
        return outcome;
    }

    outcome.hit = true;
    outcome.killed = damage.killed;
    outcome.healthAfter = damage.healthAfter;
    outcome.damageApplied = damage.damage;
    return outcome;
}

bool FinalizeContext(ShootContext &ctx,
                     const ServerPlayer &shooter,
                     const LagCompensation::LagCompFrame *lagFrame,
                     const ChunkManager &chunkManager,
                     float eyeHeight,
                     float originTolerance,
                     float originOcclusionEpsilon) {
    ctx.shooter = shooter;
    ctx.lagFrame = lagFrame;

    if (!ShootValidation::IsDirectionValid(ctx.request, ctx.rayDir)) {
        return false;
    }

    glm::vec3 shooterBasePos = shooter.position;
    if (lagFrame != nullptr) {
        const auto shooterLagIt = lagFrame->players.find(ctx.session.playerId);
        if (shooterLagIt != lagFrame->players.end()) {
            shooterBasePos = shooterLagIt->second.position;
        }
    }

    const glm::vec3 shooterEyePos = shooterBasePos + glm::vec3(0.0f, eyeHeight, 0.0f);
    const glm::vec3 requestPos(ctx.request.posX, ctx.request.posY, ctx.request.posZ);
    const ShootValidation::ValidatedOrigin validatedOrigin =
        ShootValidation::ComputeValidatedOrigin(chunkManager, shooterEyePos, requestPos,
                                                      originTolerance, originOcclusionEpsilon);
    ctx.rayOrigin = validatedOrigin.origin;
    ctx.requestedOriginAccepted = validatedOrigin.requestedOriginAccepted;
    return true;
}

void ApplyOutcomeToResult(ShootContext &ctx, const ShootOutcome &outcome) {
    ctx.result.accepted = outcome.accepted ? 1 : 0;
    ctx.result.didHit = outcome.hit ? 1 : 0;
    ctx.result.hitX = outcome.hitPoint.x;
    ctx.result.hitY = outcome.hitPoint.y;
    ctx.result.hitZ = outcome.hitPoint.z;
    if (!outcome.hit) {
        return;
    }

    ctx.result.hitEntityId =
        (outcome.hitPlayerId <= static_cast<PlayerID>(std::numeric_limits<int32_t>::max()))
            ? static_cast<int32_t>(outcome.hitPlayerId)
            : -1;
    ctx.result.damageApplied = outcome.damageApplied;
}

} // namespace Shoot
