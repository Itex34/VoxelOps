#include "CombatExecutionService.hpp"

#include "../core/LockWaitTelemetry.hpp"
#include "../../engine/physics/core/HitDetection.hpp"
#include "../../game/combat/CombatFeedback.hpp"
#include "../../game/combat/Shoot.hpp"
#include "../../game/combat/ShootValidation.hpp"
#include "../gameplay/Rules.hpp"
#include "../../../Shared/player/PlayerData.hpp"

#include <cmath>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {
constexpr float kShootMaxDistance = 128.0f;
constexpr float kShootMinIntervalSeconds = 1.0f / 8.0f;
constexpr float kShootHitboxPadXZ = 0.08f;
constexpr float kShootHitboxPadY = 0.04f;
constexpr float kShootBlockOcclusionEpsilon = 0.06f;
constexpr float kShootOriginTolerance = 0.60f;
constexpr float kShootOriginOcclusionEpsilon = 0.02f;
constexpr bool kEnableShootValidationLogs = false;

inline const Shared::PlayerData::MovementSettings &movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}
} // namespace

CombatExecutionService::CombatExecutionService(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    ChunkManager &chunkManager,
    std::atomic<uint32_t> &serverTick,
    Hooks hooks
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_chunkManager(chunkManager)
    , m_serverTick(serverTick)
    , m_hooks(std::move(hooks)) {}

const std::vector<ServerPlayerCombatSnapshot> &
CombatExecutionService::GetCombatSnapshotsForTick(uint32_t serverTick) {
    if (!m_hasCombatSnapshotsAliveCache || m_combatSnapshotsAliveCacheTick != serverTick) {
        m_combatSnapshotsAliveCache = m_playerManager.getAllCombatSnapshotsCopy(true);
        m_combatSnapshotsAliveCacheTick = serverTick;
        m_hasCombatSnapshotsAliveCache = true;
    }
    return m_combatSnapshotsAliveCache;
}

void CombatExecutionService::InvalidateCombatSnapshotCache() {
    m_combatSnapshotsAliveCache.clear();
    m_combatSnapshotsAliveCacheTick = 0;
    m_hasCombatSnapshotsAliveCache = false;
}

void CombatExecutionService::RecordLagCompFrame(uint32_t serverTick) {
    const std::vector<ServerPlayerCombatSnapshot> &players = GetCombatSnapshotsForTick(serverTick);
    LagCompensation::RecordFrame(m_lagCompFrames, serverTick, players);
}

ShootResult CombatExecutionService::ExecuteShootRequest(
    HSteamNetConnection incoming, const ShootRequest &req
) {
    if (kEnableShootValidationLogs) {
        std::cout << "[shoot/validate] recv conn=" << incoming << " shotId=" << req.clientShotId
                  << " tick=" << req.clientTick << " weapon=" << req.weaponId << " pos=("
                  << req.posX << "," << req.posY << "," << req.posZ << ")"
                  << " dir=(" << req.dirX << "," << req.dirY << "," << req.dirZ << ")"
                  << "\n";
    }

    const uint32_t currentServerTick = m_serverTick.load(std::memory_order_acquire);
    Shoot::ShootContext ctx = Shoot::BuildContext(incoming, req, currentServerTick);

    ShootValidation::ShootGateResult gate{};
    bool sessionMissing = false;
    bool unregisteredSession = false;
    {
        auto lk =
            LockWaitTelemetry::AcquireSessionLock(m_mutex, "CombatExecutionService::ExecuteShootRequest");
        const auto it = m_sessions.find(incoming);
        if (it == m_sessions.end()) {
            sessionMissing = true;
        } else {
            auto &mutableSession = it->second;
            ctx.session.username = mutableSession.username;
            ctx.session.playerId = mutableSession.playerId;
            ctx.session.registered = !ctx.session.username.empty() && ctx.session.playerId != 0;
            if (!ctx.session.registered) {
                unregisteredSession = true;
            } else {
                ShootValidation::ShootGateState gateState{};
                gateState.lastAcceptedShootTime = mutableSession.lastAcceptedShootTime;
                gateState.lastShootClientShotId = mutableSession.lastShootClientShotId;
                gateState.hasLastShootClientShotId = mutableSession.hasLastShootClientShotId;

                const float minShotIntervalSeconds =
                    Rules::MinSecondsPerShotForWeapon(req.weaponId, kShootMinIntervalSeconds);
                gate = ShootValidation::RunShootGate(gateState, req, minShotIntervalSeconds);

                mutableSession.lastAcceptedShootTime = gateState.lastAcceptedShootTime;
                mutableSession.lastShootClientShotId = gateState.lastShootClientShotId;
                mutableSession.hasLastShootClientShotId = gateState.hasLastShootClientShotId;
            }
        }
    }
    if (sessionMissing) {
        return ctx.result;
    }
    if (unregisteredSession) {
        std::cout << "[recv] ShootRequest from unregistered conn = " << incoming << "\n";
        return ctx.result;
    }

    m_playerManager.touchHeartbeat(ctx.session.playerId);
    if (!m_playerManager.setEquippedWeapon(ctx.session.playerId, req.weaponId)) {
        if (kEnableShootValidationLogs) {
            std::cout << "[shoot/validate] result=rejected"
                      << " reason=weapon_not_in_inventory"
                      << " weapon=" << req.weaponId << "\n";
        }
        return ctx.result;
    }

    if (gate.rejectedReplay || gate.rejectedCooldown) {
        if (kEnableShootValidationLogs) {
            std::cout << "[shoot/validate] result=rejected"
                      << " reason="
                      << (gate.rejectedReplay ? "replay_or_out_of_order" : "rate_limited")
                      << " shotId=" << req.clientShotId << "\n";
        }
        return ctx.result;
    }

    ctx.lagFrame =
        LagCompensation::GetFrameForTick(m_lagCompFrames, currentServerTick, req.clientTick);

    const std::optional<ServerPlayer> shooterOpt =
        m_playerManager.getPlayerCopy(ctx.session.playerId);
    if (!shooterOpt.has_value() || !shooterOpt->isAlive) {
        return ctx.result;
    }
    if (!Shoot::FinalizeContext(
            ctx,
            *shooterOpt,
            ctx.lagFrame,
            m_chunkManager,
            movementSettings().eyeHeight,
            kShootOriginTolerance,
            kShootOriginOcclusionEpsilon
        )) {
        return ctx.result;
    }

    if (kEnableShootValidationLogs) {
        std::cout << "[shoot/validate] shooter=" << ctx.session.playerId << " lagCompTick="
                  << (ctx.lagFrame ? static_cast<int64_t>(ctx.lagFrame->serverTick) : -1)
                  << " origin=(" << ctx.rayOrigin.x << "," << ctx.rayOrigin.y << ","
                  << ctx.rayOrigin.z << ")"
                  << " requestedOriginAccepted=" << (ctx.requestedOriginAccepted ? "yes" : "no")
                  << " maxDistance=" << kShootMaxDistance << "\n";
    }

    const std::vector<ServerPlayerCombatSnapshot> &players =
        GetCombatSnapshotsForTick(currentServerTick);

    HitDetection::HitDetectionInput detectionInput{};
    detectionInput.shooterId = ctx.session.playerId;
    detectionInput.rayOrigin = ctx.rayOrigin;
    detectionInput.rayDir = ctx.rayDir;
    detectionInput.maxDistance = kShootMaxDistance;
    detectionInput.hitboxPadXZ = kShootHitboxPadXZ;
    detectionInput.hitboxPadY = kShootHitboxPadY;
    detectionInput.chunkManager = &m_chunkManager;
    detectionInput.lagCompFrame = ctx.lagFrame;
    detectionInput.players = &players;
    detectionInput.enableValidationLogs = kEnableShootValidationLogs;

    const HitDetection::HitDetectionResult hitResult =
        HitDetection::RaycastPlayersAndWorld(detectionInput);

    if (kEnableShootValidationLogs) {
        std::cout << "[shoot/validate] nearest playerHit=" << (hitResult.playerHit ? "yes" : "no")
                  << " playerDist=" << (hitResult.playerHit ? hitResult.bestPlayerDistance : -1.0f)
                  << " blockHit=" << (hitResult.worldRaycastResult.hit ? "yes" : "no")
                  << " blockDist="
                  << (hitResult.worldRaycastResult.hit ? hitResult.worldRaycastResult.distance
                                                       : -1.0f)
                  << "\n";
    }

    const Shoot::ShootOutcome outcome =
        Shoot::ResolveHit(ctx, hitResult, kShootBlockOcclusionEpsilon, m_playerManager);
    Shoot::ApplyOutcomeToResult(ctx, outcome);

    if (kEnableShootValidationLogs) {
        if (!outcome.hit) {
            const char *reason =
                (!hitResult.playerHit) ? "no_player_intersection" : "miss_or_occluded";
            std::cout << "[shoot/validate] result=miss reason=" << reason
                      << " blockDist="
                      << (hitResult.worldRaycastResult.hit ? hitResult.worldRaycastResult.distance
                                                           : -1.0f)
                      << " playerDist=" << (hitResult.playerHit ? hitResult.bestPlayerDistance : -1.0f)
                      << " epsilon=" << kShootBlockOcclusionEpsilon << " endpoint=("
                      << outcome.hitPoint.x << "," << outcome.hitPoint.y << ","
                      << outcome.hitPoint.z << ")"
                      << "\n";
        } else {
            std::cout << "[shoot/validate] result=hit"
                      << " target=" << outcome.hitPlayerId
                      << " region=" << Rules::HitRegionName(outcome.hitRegion)
                      << " damage=" << outcome.damageApplied
                      << " healthAfter=" << outcome.healthAfter
                      << " killed=" << (outcome.killed ? "yes" : "no") << " point=("
                      << outcome.hitPoint.x << "," << outcome.hitPoint.y << ","
                      << outcome.hitPoint.z << ")"
                      << "\n";
        }
    }

    if (outcome.hit && outcome.killed) {
        m_hooks.onConfirmedKill(ctx.session.playerId, ctx.session.username, outcome.hitPlayerId, req.weaponId);
    }

    return ctx.result;
}

GrappleResult CombatExecutionService::ExecuteGrappleRequest(
    HSteamNetConnection incoming, const GrappleRequest &req
) {
    GrappleResult result{};
    result.clientGrappleId = req.clientGrappleId;
    result.serverTick = m_serverTick.load(std::memory_order_acquire);
    result.serverSeed = req.seed;
    result.faceNormal = 255;

    PlayerID playerId = 0;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "CombatExecutionService::ExecuteGrappleRequest"
        );
        const auto it = m_sessions.find(incoming);
        if (it == m_sessions.end()) {
            return result;
        }
        const auto &session = it->second;
        if (session.username.empty() || session.playerId == 0) {
            std::cout << "[recv] GrappleRequest from unregistered conn = " << incoming << "\n";
            return result;
        }
        playerId = session.playerId;
    }

    m_playerManager.touchHeartbeat(playerId);

    const glm::vec3 requestedDirection(req.dirX, req.dirY, req.dirZ);
    const float dirLenSq =
        (requestedDirection.x * requestedDirection.x) +
        (requestedDirection.y * requestedDirection.y) +
        (requestedDirection.z * requestedDirection.z);
    const double nowSeconds = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now().time_since_epoch()
    )
                                  .count();

    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        if (m_playerManager.releaseGrapple(playerId)) {
            result.accepted = 1u;
        }
        return result;
    }

    const bool controlPacket =
        std::abs(req.posX) < 1e-6f && std::abs(req.posY) < 1e-6f && std::abs(req.posZ) < 1e-6f;

    if (controlPacket) {
        if (m_playerManager.setGrappleReeling(playerId, true, nowSeconds)) {
            result.accepted = 1u;
        }
        return result;
    }

    const std::optional<ServerPlayer> shooterOpt = m_playerManager.getPlayerCopy(playerId);
    if (!shooterOpt.has_value() || !shooterOpt->isAlive) {
        return result;
    }

    const glm::vec3 origin =
        shooterOpt->position + glm::vec3(0.0f, movementSettings().eyeHeight, 0.0f);
    const glm::vec3 direction = requestedDirection / std::sqrt(dirLenSq);

    GrappleFireResult fireResult{};
    if (!m_playerManager.tryFireGrapple(
            playerId, origin, direction, nowSeconds, m_chunkManager, fireResult
        )) {
        return result;
    }

    result.accepted = fireResult.accepted ? 1u : 0u;
    result.didHit = fireResult.attached ? 1u : 0u;
    if (!fireResult.attached) {
        return result;
    }

    result.hitX = fireResult.anchor.x;
    result.hitY = fireResult.anchor.y;
    result.hitZ = fireResult.anchor.z;
    result.faceNormal = fireResult.blockNormal;
    return result;
}
