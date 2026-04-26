#include "../core/ServerRuntime.hpp"

#include "CombatRules.hpp"
#include "../protocol/PacketParsers.hpp"
#include "../../gameplay/combat/CombatFeedbackSystem.hpp"
#include "../../gameplay/combat/LagCompensationSystem.hpp"
#include "../../gameplay/combat/ShootSystem.hpp"
#include "../../gameplay/combat/ShootValidationSystem.hpp"
#include "../../physics/HitDetectionSystem.hpp"
#include "../../../Shared/player/PlayerData.hpp"

#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
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

void SendShootResult(HSteamNetConnection incoming, const ShootResult &result) {
    const std::vector<uint8_t> outBuf = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming, outBuf.data(), static_cast<uint32_t>(outBuf.size()),
        k_nSteamNetworkingSend_Reliable, nullptr);
}
} // namespace




const std::vector<ServerPlayerCombatSnapshot> &
ServerRuntime::GetCombatSnapshotsForTick(uint32_t serverTick) {
    if (!m_hasCombatSnapshotsAliveCache || m_combatSnapshotsAliveCacheTick != serverTick) {
        m_combatSnapshotsAliveCache = m_playerManager.getAllCombatSnapshotsCopy(true);
        m_combatSnapshotsAliveCacheTick = serverTick;
        m_hasCombatSnapshotsAliveCache = true;
    }
    return m_combatSnapshotsAliveCache;
}

void ServerRuntime::InvalidateCombatSnapshotCache() {
    m_combatSnapshotsAliveCache.clear();
    m_combatSnapshotsAliveCacheTick = 0;
    m_hasCombatSnapshotsAliveCache = false;
}

void ServerRuntime::RecordLagCompFrame(uint32_t serverTick) {
    const std::vector<ServerPlayerCombatSnapshot> &players = GetCombatSnapshotsForTick(serverTick);
    LagCompensationSystem::RecordFrame(m_lagCompFrames, serverTick, players);
}

void ServerRuntime::HandleShootRequestPacket(HSteamNetConnection incoming, const void *data,
                                             uint32_t size) {
    ShootRequest req{};
    if (!NetPacket::ParseShootRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cerr << "[recv] malformed ShootRequest\n";
        return;
    }
    if (kEnableShootValidationLogs) {
        std::cout << "[shoot/validate] recv conn=" << incoming << " shotId=" << req.clientShotId
                  << " tick=" << req.clientTick << " weapon=" << req.weaponId << " pos=("
                  << req.posX << "," << req.posY << "," << req.posZ << ")"
                  << " dir=(" << req.dirX << "," << req.dirY << "," << req.dirZ << ")"
                  << "\n";
    }

    const uint32_t currentServerTick = m_serverTick.load(std::memory_order_acquire);
    ShootSystem::ShootContext ctx = ShootSystem::BuildContext(incoming, req, currentServerTick);

    // Stage 1 + 3: validate session and run anti-replay / cooldown gate.
    ShootValidationSystem::ShootGateResult gate{};
    bool sessionMissing = false;
    bool unregisteredSession = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto it = m_clients.find(incoming);
        if (it == m_clients.end()) {
            sessionMissing = true;
        } else {
            ServerRuntime::ClientSession &mutableSession = it->second;
            ctx.session.username = mutableSession.username;
            ctx.session.playerId = mutableSession.playerId;
            ctx.session.registered = !ctx.session.username.empty() && ctx.session.playerId != 0;
            if (!ctx.session.registered) {
                unregisteredSession = true;
            } else {
                ShootValidationSystem::ShootGateState gateState{};
                gateState.lastAcceptedShootTime = mutableSession.lastAcceptedShootTime;
                gateState.lastShootClientShotId = mutableSession.lastShootClientShotId;
                gateState.hasLastShootClientShotId = mutableSession.hasLastShootClientShotId;

                const float minShotIntervalSeconds =
                    CombatRules::MinSecondsPerShotForWeapon(req.weaponId, kShootMinIntervalSeconds);
                gate = ShootValidationSystem::RunShootGate(gateState, req, minShotIntervalSeconds);

                mutableSession.lastAcceptedShootTime = gateState.lastAcceptedShootTime;
                mutableSession.lastShootClientShotId = gateState.lastShootClientShotId;
                mutableSession.hasLastShootClientShotId = gateState.hasLastShootClientShotId;
            }
        }
    }
    if (sessionMissing) {
        SendShootResult(incoming, ctx.result);
        return;
    }
    if (unregisteredSession) {
        std::cout << "[recv] ShootRequest from unregistered conn = " << incoming << "\n";
        SendShootResult(incoming, ctx.result);
        return;
    }

    // Stage 2: weapon sync + heartbeat.
    m_playerManager.touchHeartbeat(ctx.session.playerId);
    if (!m_playerManager.setEquippedWeapon(ctx.session.playerId, req.weaponId)) {
        if (kEnableShootValidationLogs) {
            std::cout << "[shoot/validate] result=rejected"
                      << " reason=weapon_not_in_inventory"
                      << " weapon=" << req.weaponId << "\n";
        }
        SendShootResult(incoming, ctx.result);
        return;
    }

    if (gate.rejectedReplay || gate.rejectedCooldown) {
        if (kEnableShootValidationLogs) {
            std::cout << "[shoot/validate] result=rejected"
                      << " reason="
                      << (gate.rejectedReplay ? "replay_or_out_of_order" : "rate_limited")
                      << " shotId=" << req.clientShotId << "\n";
        }
        SendShootResult(incoming, ctx.result);
        return;
    }

    // Stage 4: lag compensation frame.
    ctx.lagFrame =
        LagCompensationSystem::GetFrameForTick(m_lagCompFrames, currentServerTick, req.clientTick);

    // Stage 5: build ray context.
    const std::optional<ServerPlayer> shooterOpt = m_playerManager.getPlayerCopy(ctx.session.playerId);
    if (!shooterOpt.has_value() || !shooterOpt->isAlive) {
        SendShootResult(incoming, ctx.result);
        return;
    }
    if (!ShootSystem::FinalizeContext(ctx, *shooterOpt, ctx.lagFrame, m_chunkManager,
                                      movementSettings().eyeHeight, kShootOriginTolerance,
                                      kShootOriginOcclusionEpsilon)) {
        SendShootResult(incoming, ctx.result);
        return;
    }

    if (kEnableShootValidationLogs) {
        std::cout << "[shoot/validate] shooter=" << ctx.session.playerId << " lagCompTick="
                  << (ctx.lagFrame ? static_cast<int64_t>(ctx.lagFrame->serverTick) : -1)
                  << " origin=(" << ctx.rayOrigin.x << "," << ctx.rayOrigin.y << ","
                  << ctx.rayOrigin.z << ")"
                  << " requestedOriginAccepted="
                  << (ctx.requestedOriginAccepted ? "yes" : "no")
                  << " maxDistance=" << kShootMaxDistance << "\n";
    }

    // Stage 6: hit detection.
    const std::vector<ServerPlayerCombatSnapshot> &players =
        GetCombatSnapshotsForTick(currentServerTick);

    HitDetectionSystem::HitDetectionInput detectionInput{};
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

    const HitDetectionSystem::HitDetectionResult hit =
        HitDetectionSystem::RaycastPlayersAndWorld(detectionInput);

    if (kEnableShootValidationLogs) {
        std::cout << "[shoot/validate] nearest playerHit=" << (hit.playerHit ? "yes" : "no")
                  << " playerDist=" << (hit.playerHit ? hit.bestPlayerDistance : -1.0f)
                  << " blockHit=" << (hit.blockHit ? "yes" : "no")
                  << " blockDist=" << (hit.blockHit ? hit.blockDistance : -1.0f) << "\n";
    }

    // Stage 7: resolve hit and apply damage.
    const ShootSystem::ShootOutcome outcome =
        ShootSystem::ResolveHit(ctx, hit, kShootBlockOcclusionEpsilon, m_playerManager);
    ShootSystem::ApplyOutcomeToResult(ctx, outcome);

    if (kEnableShootValidationLogs) {
        if (!outcome.hit) {
            const char *reason = (!hit.playerHit) ? "no_player_intersection" : "miss_or_occluded";
            std::cout << "[shoot/validate] result=miss reason=" << reason
                      << " blockDist=" << (hit.blockHit ? hit.blockDistance : -1.0f)
                      << " playerDist=" << (hit.playerHit ? hit.bestPlayerDistance : -1.0f)
                      << " epsilon=" << kShootBlockOcclusionEpsilon << " endpoint=("
                      << outcome.hitPoint.x << "," << outcome.hitPoint.y << ","
                      << outcome.hitPoint.z << ")"
                      << "\n";
        } else {
            std::cout << "[shoot/validate] result=hit"
                      << " target=" << outcome.hitPlayerId
                      << " region=" << CombatRules::HitRegionName(outcome.hitRegion)
                      << " damage=" << outcome.damageApplied
                      << " healthAfter=" << outcome.healthAfter
                      << " killed=" << (outcome.killed ? "yes" : "no")
                      << " point=(" << outcome.hitPoint.x << "," << outcome.hitPoint.y << ","
                      << outcome.hitPoint.z << ")"
                      << "\n";
        }
    }

    // Stage 8: side-effects (score + killfeed).
    if (outcome.hit && outcome.killed) {
        std::string victimUsername;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            CombatFeedbackSystem::ApplyKillScore(m_matchScores, m_matchEnded, ctx.session.playerId,
                                                 outcome.hitPlayerId);
            for (const auto &[_, clientSession] : m_clients) {
                if (clientSession.playerId == outcome.hitPlayerId) {
                    victimUsername = clientSession.username;
                    break;
                }
            }
        }

        if (victimUsername.empty()) {
            victimUsername = CombatFeedbackSystem::FallbackVictimUsername(outcome.hitPlayerId);
        }

        const std::string killfeedPacket =
            CombatFeedbackSystem::BuildKillfeedPacket(ctx.session.username, victimUsername,
                                                      req.weaponId);
        BroadcastRaw(killfeedPacket.data(), static_cast<uint32_t>(killfeedPacket.size()),
                     k_HSteamNetConnection_Invalid);
    }

    // Stage 9: send result.
    SendShootResult(incoming, ctx.result);
}
