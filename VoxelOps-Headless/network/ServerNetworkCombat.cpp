#include "ServerNetwork.hpp"
#include "PacketParsers.hpp"
#include "CombatRules.hpp"
#include "../player/Hitbox.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/player/HitboxCache.hpp"
#include "../../Shared/player/MeshHitCache.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/vec3.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>

namespace {
constexpr uint32_t kServerTickRateHz = 60u;
constexpr float kShootMaxDistance = 128.0f;
constexpr float kShootMinIntervalSeconds = 1.0f / 8.0f;
constexpr float kShootLagCompensationWindowSeconds = 0.300f;
constexpr uint32_t kShootLagCompensationMaxTicks =
    static_cast<uint32_t>(kShootLagCompensationWindowSeconds * static_cast<float>(kServerTickRateHz) + 0.5f);
constexpr size_t kShootLagCompensationMaxFrames =
    static_cast<size_t>(kShootLagCompensationMaxTicks + 4u);
constexpr float kShootHitboxPadXZ = 0.08f;
constexpr float kShootHitboxPadY = 0.04f;
constexpr float kShootBlockOcclusionEpsilon = 0.06f;
constexpr float kShootOriginTolerance = 0.60f;
constexpr float kShootOriginOcclusionEpsilon = 0.02f;
constexpr bool kEnableHitboxDiagnostics = true;
constexpr bool kEnableShootValidationLogs = false;

const std::string& SharedHitboxCachePath() {
    static const std::string kPath =
        Shared::RuntimePaths::ResolveSharedPath("generated/player_hitboxes.bin").generic_string();
    return kPath;
}

const std::string& SharedMeshHitCachePath() {
    static const std::string kPath =
        Shared::RuntimePaths::ResolveSharedPath("generated/player_mesh_hit.bin").generic_string();
    return kPath;
}

inline const Shared::PlayerData::MovementSettings& movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}

inline bool IsNewerU32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

struct MeshHitTriangle {
    glm::vec3 a{ 0.0f };
    glm::vec3 b{ 0.0f };
    glm::vec3 c{ 0.0f };
    HitRegion region = HitRegion::Body;
};

struct CachedTransform {
    glm::mat4 model{ 1.0f };
    glm::mat4 invModel{ 1.0f };
};

struct MeshBroadphaseBounds {
    glm::vec3 centerLocal{ 0.0f };
    float radiusLocal = 0.0f;
    bool valid = false;
};

HitRegion RegionFromLocalHeight(float localY, float height) {
    if (!std::isfinite(localY) || !std::isfinite(height) || height <= 1e-4f) {
        return HitRegion::Unknown;
    }
    const float n = std::clamp(localY / height, 0.0f, 1.0f);
    if (n >= 0.72f) {
        return HitRegion::Head;
    }
    if (n >= 0.40f) {
        return HitRegion::Body;
    }
    return HitRegion::Legs;
}

HitRegion ResolveHitRegionByPoint(
    const glm::vec3& hitPointWorld,
    const glm::mat4& invModelMatrix,
    float height,
    HitRegion fallback
) {
    const glm::vec3 hitPointLocal = glm::vec3(invModelMatrix * glm::vec4(hitPointWorld, 1.0f));
    const HitRegion byHeight = RegionFromLocalHeight(hitPointLocal.y, height);
    return (byHeight == HitRegion::Unknown) ? fallback : byHeight;
}

const std::vector<Hitbox>& GetSharedPlayerHitboxes(float& outReferenceHeight, float& outReferenceRadius) {
    static std::once_flag initFlag;
    static float referenceHeight = movementSettings().collisionHeight;
    static float referenceRadius = movementSettings().collisionRadius;
    static std::vector<Hitbox> cachedHitboxes;

    std::call_once(initFlag, [&]() {
        float loadedHeight = 0.0f;
        float loadedRadius = 0.0f;
        std::vector<Shared::HitboxCache::Record> records;
        if (Shared::HitboxCache::Load(SharedHitboxCachePath(), loadedHeight, loadedRadius, records) && !records.empty()) {
            cachedHitboxes.reserve(records.size());
            for (const Shared::HitboxCache::Record& rec : records) {
                Hitbox hb;
                hb.min = glm::vec3(rec.minX, rec.minY, rec.minZ);
                hb.max = glm::vec3(rec.maxX, rec.maxY, rec.maxZ);
                hb.region = CombatRules::RegionFromCacheCode(rec.region);
                cachedHitboxes.push_back(hb);
            }
            referenceHeight = (loadedHeight > 1e-4f) ? loadedHeight : movementSettings().collisionHeight;
            referenceRadius = (loadedRadius > 1e-4f) ? loadedRadius : movementSettings().collisionRadius;
            std::cout << "[hitbox] loaded shared cache: " << SharedHitboxCachePath() << "\n";
            if (kEnableHitboxDiagnostics) {
                std::cout
                    << "[hitbox/server] source=cache count=" << cachedHitboxes.size()
                    << " refHeight=" << referenceHeight
                    << " refRadius=" << referenceRadius
                    << "\n";
                for (size_t i = 0; i < cachedHitboxes.size(); ++i) {
                    const Hitbox& hb = cachedHitboxes[i];
                    std::cout
                        << "  [" << i << "] region=" << CombatRules::HitRegionName(hb.region)
                        << " min=(" << hb.min.x << "," << hb.min.y << "," << hb.min.z << ")"
                        << " max=(" << hb.max.x << "," << hb.max.y << "," << hb.max.z << ")"
                        << "\n";
                }
            }
        }

        if (cachedHitboxes.empty()) {
            cachedHitboxes = HitboxManager::buildBlockyHitboxes(
                movementSettings().collisionHeight,
                movementSettings().collisionRadius,
                movementSettings().collisionRadius,
                true
            );
            std::cerr << "[hitbox] cache missing/unusable, using fallback procedural hitboxes.\n";
            if (kEnableHitboxDiagnostics) {
                std::cout
                    << "[hitbox/server] source=fallback count=" << cachedHitboxes.size()
                    << " refHeight=" << referenceHeight
                    << " refRadius=" << referenceRadius
                    << "\n";
            }
        }
    });

    outReferenceHeight = referenceHeight;
    outReferenceRadius = referenceRadius;
    return cachedHitboxes;
}

const std::vector<MeshHitTriangle>& GetSharedPlayerMeshHitTriangles(
    float& outReferenceHeight,
    MeshBroadphaseBounds* outBroadphaseBounds
) {
    static std::once_flag initFlag;
    static float referenceHeight = movementSettings().collisionHeight;
    static std::vector<MeshHitTriangle> cachedTriangles;
    static MeshBroadphaseBounds cachedBounds;

    std::call_once(initFlag, [&]() {
        float loadedHeight = 0.0f;
        std::vector<Shared::MeshHitCache::TriangleRecord> records;
        if (Shared::MeshHitCache::Load(SharedMeshHitCachePath(), loadedHeight, records) && !records.empty()) {
            cachedTriangles.reserve(records.size());
            for (const Shared::MeshHitCache::TriangleRecord& rec : records) {
                MeshHitTriangle tri;
                tri.a = glm::vec3(rec.ax, rec.ay, rec.az);
                tri.b = glm::vec3(rec.bx, rec.by, rec.bz);
                tri.c = glm::vec3(rec.cx, rec.cy, rec.cz);
                tri.region = CombatRules::RegionFromCacheCode(rec.region);
                cachedTriangles.push_back(tri);
            }
            if (loadedHeight > 1e-4f) {
                referenceHeight = loadedHeight;
            }

            glm::vec3 minP(std::numeric_limits<float>::max());
            glm::vec3 maxP(std::numeric_limits<float>::lowest());
            for (const MeshHitTriangle& tri : cachedTriangles) {
                minP = glm::min(minP, tri.a);
                minP = glm::min(minP, tri.b);
                minP = glm::min(minP, tri.c);
                maxP = glm::max(maxP, tri.a);
                maxP = glm::max(maxP, tri.b);
                maxP = glm::max(maxP, tri.c);
            }

            cachedBounds.centerLocal = 0.5f * (minP + maxP);
            float radiusSq = 0.0f;
            for (const MeshHitTriangle& tri : cachedTriangles) {
                const glm::vec3 da = tri.a - cachedBounds.centerLocal;
                const glm::vec3 db = tri.b - cachedBounds.centerLocal;
                const glm::vec3 dc = tri.c - cachedBounds.centerLocal;
                radiusSq = std::max(radiusSq, glm::dot(da, da));
                radiusSq = std::max(radiusSq, glm::dot(db, db));
                radiusSq = std::max(radiusSq, glm::dot(dc, dc));
            }
            cachedBounds.radiusLocal = std::sqrt(radiusSq);
            cachedBounds.valid = std::isfinite(cachedBounds.radiusLocal) && cachedBounds.radiusLocal > 1e-4f;

            if (kEnableHitboxDiagnostics) {
                std::cout
                    << "[hitbox/server] mesh_source=cache triangles=" << cachedTriangles.size()
                    << " refHeight=" << referenceHeight
                    << " broadphaseRadius=" << cachedBounds.radiusLocal
                    << " path=" << SharedMeshHitCachePath()
                    << "\n";
            }
        }
        else if (kEnableHitboxDiagnostics) {
            std::cout
                << "[hitbox/server] mesh_source=missing path=" << SharedMeshHitCachePath()
                << " (falling back to AABB hitboxes)\n";
        }
    });

    outReferenceHeight = referenceHeight;
    if (outBroadphaseBounds != nullptr) {
        *outBroadphaseBounds = cachedBounds;
    }
    return cachedTriangles;
}

bool RayIntersectsTriangle(
    const glm::vec3& origin,
    const glm::vec3& dir,
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c,
    float& outT
) {
    constexpr float kEpsilon = 1e-6f;
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 pvec = glm::cross(dir, edge2);
    const float det = glm::dot(edge1, pvec);
    if (std::abs(det) < kEpsilon) {
        return false;
    }
    const float invDet = 1.0f / det;
    const glm::vec3 tvec = origin - a;
    const float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(dir, qvec) * invDet;
    if (v < 0.0f || (u + v) > 1.0f) {
        return false;
    }
    const float t = glm::dot(edge2, qvec) * invDet;
    if (t <= kEpsilon) {
        return false;
    }
    outT = t;
    return true;
}

bool RayIntersectsSphere(
    const glm::vec3& origin,
    const glm::vec3& dirNorm,
    const glm::vec3& center,
    float radius,
    float maxDistance,
    float& outNearT
) {
    if (!std::isfinite(radius) || radius <= 1e-6f) {
        return false;
    }
    const glm::vec3 m = origin - center;
    const float b = glm::dot(m, dirNorm);
    const float c = glm::dot(m, m) - radius * radius;
    if (c > 0.0f && b > 0.0f) {
        return false;
    }
    const float disc = b * b - c;
    if (disc < 0.0f) {
        return false;
    }
    float t = -b - std::sqrt(disc);
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (!std::isfinite(t) || t > maxDistance) {
        return false;
    }
    outNearT = t;
    return true;
}

HitResult RaycastMeshTriangles(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDir,
    const std::vector<MeshHitTriangle>& triangles,
    const CachedTransform& transform,
    float uniformScale,
    float maxDistance
) {
    HitResult out;
    out.hit = false;
    out.region = HitRegion::Unknown;
    out.distance = maxDistance;

    const glm::vec3 originLocal = glm::vec3(transform.invModel * glm::vec4(rayOrigin, 1.0f));
    glm::vec3 dirLocal = glm::vec3(transform.invModel * glm::vec4(rayDir, 0.0f));
    const float dirLen = glm::length(dirLocal);
    if (dirLen <= 1e-8f) {
        return out;
    }
    dirLocal /= dirLen;

    for (const MeshHitTriangle& tri : triangles) {
        const glm::vec3 a = tri.a * uniformScale;
        const glm::vec3 b = tri.b * uniformScale;
        const glm::vec3 c = tri.c * uniformScale;
        float t = 0.0f;
        if (!RayIntersectsTriangle(originLocal, dirLocal, a, b, c, t)) {
            continue;
        }
        if (t > maxDistance) {
            continue;
        }
        if (!out.hit || t < out.distance) {
            out.hit = true;
            out.distance = t;
            out.region = tri.region;
            const glm::vec3 hitLocal = originLocal + dirLocal * t;
            out.hitPointWorld = glm::vec3(transform.model * glm::vec4(hitLocal, 1.0f));
        }
    }

    return out;
}

CachedTransform BuildPlayerTransform(const glm::vec3& position, float yaw) {
    CachedTransform transform;
    transform.model = glm::translate(glm::mat4(1.0f), position);
    transform.model = glm::rotate(
        transform.model,
        glm::radians(CombatRules::ToModelYawDegrees(yaw)),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    transform.invModel = glm::affineInverse(transform.model);
    return transform;
}

bool FindFirstSolidBlockHit(
    const ChunkManager& chunkManager,
    const glm::vec3& origin,
    const glm::vec3& dir,
    float maxDistance,
    float& outDistance,
    glm::vec3& outHitPoint
) {
    if (!std::isfinite(maxDistance) || maxDistance <= 0.0f) {
        return false;
    }
    const float dirLenSq = glm::dot(dir, dir);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        return false;
    }

    const glm::vec3 rayDir = glm::normalize(dir);
    glm::ivec3 currentBlock = glm::ivec3(glm::floor(origin));
    const glm::ivec3 step = glm::sign(rayDir);

    glm::vec3 tMax(0.0f);
    glm::vec3 tDelta(0.0f);
    for (int i = 0; i < 3; ++i) {
        if (rayDir[i] != 0.0f) {
            const float nextBoundary = (step[i] > 0) ? (currentBlock[i] + 1.0f) : currentBlock[i];
            tMax[i] = (nextBoundary - origin[i]) / rayDir[i];
            tDelta[i] = std::abs(1.0f / rayDir[i]);
        }
        else {
            tMax[i] = std::numeric_limits<float>::max();
            tDelta[i] = std::numeric_limits<float>::max();
        }
    }

    constexpr int kMaxDdaSteps = 2048;
    float traveled = 0.0f;
    for (int i = 0; i < kMaxDdaSteps; ++i) {
        if (traveled > maxDistance) {
            break;
        }

        const glm::ivec3 chunkCoords = chunkManager.worldToChunkPos(currentBlock);
        if (const ServerChunk* chunk = chunkManager.getChunkIfExists(chunkCoords)) {
            const glm::ivec3 blockInChunk = currentBlock - chunk->getWorldPosition();
            if (ServerChunk::inBounds(blockInChunk.x, blockInChunk.y, blockInChunk.z)) {
                if (chunk->getBlockUnchecked(blockInChunk.x, blockInChunk.y, blockInChunk.z) != BlockID::Air) {
                    outDistance = traveled;
                    outHitPoint = origin + rayDir * traveled;
                    return true;
                }
            }
        }

        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                traveled = tMax.x;
                currentBlock.x += step.x;
                tMax.x += tDelta.x;
            }
            else {
                traveled = tMax.z;
                currentBlock.z += step.z;
                tMax.z += tDelta.z;
            }
        }
        else {
            if (tMax.y < tMax.z) {
                traveled = tMax.y;
                currentBlock.y += step.y;
                tMax.y += tDelta.y;
            }
            else {
                traveled = tMax.z;
                currentBlock.z += step.z;
                tMax.z += tDelta.z;
            }
        }
    }

    return false;
}

}

void ServerNetwork::RecordLagCompFrame(uint32_t serverTick)
{
    LagCompFrame frame{};
    frame.serverTick = serverTick;

    const std::vector<ServerPlayerCombatSnapshot> players = m_playerManager.getAllCombatSnapshotsCopy(true);
    frame.players.reserve(players.size());
    for (const ServerPlayerCombatSnapshot& player : players) {
        LagCompPlayerPose pose{};
        pose.position = player.position;
        pose.yaw = player.yaw;
        pose.height = player.height;
        pose.radius = player.radius;
        frame.players.emplace(player.id, pose);
    }

    m_lagCompFrames.push_back(std::move(frame));
    while (m_lagCompFrames.size() > kShootLagCompensationMaxFrames) {
        m_lagCompFrames.pop_front();
    }
    while (!m_lagCompFrames.empty()) {
        const uint32_t oldestTick = m_lagCompFrames.front().serverTick;
        if (!IsNewerU32(serverTick, oldestTick)) {
            break;
        }
        if ((serverTick - oldestTick) <= kShootLagCompensationMaxTicks) {
            break;
        }
        m_lagCompFrames.pop_front();
    }
}

void ServerNetwork::HandleShootRequestPacket(HSteamNetConnection incoming, const void* data, uint32_t size)
{
    auto sendResult = [&](const ShootResult& res) {
        const std::vector<uint8_t> outBuf = res.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming,
            outBuf.data(),
            static_cast<uint32_t>(outBuf.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    };

    auto makeBaseRejectedResult = [&](uint32_t shotId, uint32_t serverTick, uint32_t seed) {
        ShootResult result{};
        result.clientShotId = shotId;
        result.serverTick = serverTick;
        result.accepted = 0;
        result.didHit = 0;
        result.hitEntityId = -1;
        result.serverSeed = seed;
        result.newAmmoCount = 0;
        return result;
    };

    ShootRequest req{};
    if (!NetPacket::ParseShootRequestPacket(reinterpret_cast<const uint8_t*>(data), size, req)) {
        std::cerr << "[recv] malformed ShootRequest\n";
        return;
    }
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] recv conn=" << incoming
            << " shotId=" << req.clientShotId
            << " tick=" << req.clientTick
            << " weapon=" << req.weaponId
            << " pos=(" << req.posX << "," << req.posY << "," << req.posZ << ")"
            << " dir=(" << req.dirX << "," << req.dirY << "," << req.dirZ << ")"
            << "\n";
    }

    const uint32_t currentServerTick = m_serverTick.load(std::memory_order_acquire);
    ShootResult res = makeBaseRejectedResult(req.clientShotId, currentServerTick, req.seed);

    struct ShootSessionSnapshot {
        std::string username;
        PlayerID playerId = 0;
        bool registered = false;
    };

    const auto snapshotSession = [&]() -> ShootSessionSnapshot {
        ShootSessionSnapshot snapshot{};
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto it = m_clients.find(incoming);
        if (it == m_clients.end()) {
            return snapshot;
        }
        snapshot.username = it->second.username;
        snapshot.playerId = it->second.playerId;
        snapshot.registered = !snapshot.username.empty() && snapshot.playerId != 0;
        return snapshot;
    };

    const ShootSessionSnapshot session = snapshotSession();
    if (!session.registered) {
        std::cout << "[recv] ShootRequest from unregistered conn = " << incoming << "\n";
        sendResult(res);
        return;
    }

    // Step 1: keep heartbeat/facing weapon state in sync with incoming shot stream.
    m_playerManager.touchHeartbeat(session.playerId);
    if (!m_playerManager.setEquippedWeapon(session.playerId, req.weaponId)) {
        if (kEnableShootValidationLogs) {
            std::cout
                << "[shoot/validate] result=rejected"
                << " reason=weapon_not_in_inventory"
                << " weapon=" << req.weaponId
                << "\n";
        }
        sendResult(res);
        return;
    }

    struct ShootGateResult {
        bool sessionMissing = false;
        bool rejectedReplay = false;
        bool rejectedCooldown = false;
    };

    // Step 2: replay/rate gate.
    const auto runShootGate = [&]() -> ShootGateResult {
        ShootGateResult gate{};
        const float minShotIntervalSeconds =
            CombatRules::MinSecondsPerShotForWeapon(req.weaponId, kShootMinIntervalSeconds);
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto it = m_clients.find(incoming);
        if (it == m_clients.end()) {
            gate.sessionMissing = true;
            return gate;
        }

        ClientSession& mutableSession = it->second;
        if (mutableSession.hasLastShootClientShotId &&
            !IsNewerU32(req.clientShotId, mutableSession.lastShootClientShotId)) {
            gate.rejectedReplay = true;
            return gate;
        }

        mutableSession.lastShootClientShotId = req.clientShotId;
        mutableSession.hasLastShootClientShotId = true;

        const auto now = std::chrono::steady_clock::now();
        if (mutableSession.lastAcceptedShootTime != std::chrono::steady_clock::time_point::min()) {
            const float elapsedSeconds = std::chrono::duration<float>(
                now - mutableSession.lastAcceptedShootTime
            ).count();
            if (elapsedSeconds < minShotIntervalSeconds) {
                gate.rejectedCooldown = true;
                return gate;
            }
        }
        mutableSession.lastAcceptedShootTime = now;
        return gate;
    };

    const ShootGateResult gate = runShootGate();
    if (gate.sessionMissing) {
        sendResult(res);
        return;
    }
    if (gate.rejectedReplay || gate.rejectedCooldown) {
        if (kEnableShootValidationLogs) {
            std::cout
                << "[shoot/validate] result=rejected"
                << " reason=" << (gate.rejectedReplay ? "replay_or_out_of_order" : "rate_limited")
                << " shotId=" << req.clientShotId
                << "\n";
        }
        sendResult(res);
        return;
    }

    // Step 3: lag-comp frame lookup.
    const auto getLagCompFrameForTick = [&]() -> const LagCompFrame* {
        uint32_t lagCompTargetTick = currentServerTick;
        if (req.clientTick <= currentServerTick &&
            (currentServerTick - req.clientTick) <= kShootLagCompensationMaxTicks) {
            lagCompTargetTick = req.clientTick;
        }
        if (m_lagCompFrames.empty()) {
            return nullptr;
        }
        for (auto it = m_lagCompFrames.rbegin(); it != m_lagCompFrames.rend(); ++it) {
            if (!IsNewerU32(it->serverTick, lagCompTargetTick)) {
                return &(*it);
            }
        }
        return nullptr;
    };
    const LagCompFrame* lagCompFrame = getLagCompFrameForTick();

    auto shooterOpt = m_playerManager.getPlayerCopy(session.playerId);
    if (!shooterOpt.has_value()) {
        sendResult(res);
        return;
    }
    const ServerPlayer& shooter = *shooterOpt;
    if (!shooter.isAlive) {
        sendResult(res);
        return;
    }

    const glm::vec3 requestDir(req.dirX, req.dirY, req.dirZ);
    const float dirLenSq = glm::dot(requestDir, requestDir);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        sendResult(res);
        return;
    }

    // Step 4: build shoot context.
    glm::vec3 shooterBasePos = shooter.position;
    if (lagCompFrame != nullptr) {
        auto shooterLagIt = lagCompFrame->players.find(session.playerId);
        if (shooterLagIt != lagCompFrame->players.end()) {
            shooterBasePos = shooterLagIt->second.position;
        }
    }

    const glm::vec3 rayDir = glm::normalize(requestDir);
    const glm::vec3 shooterEyePos = shooterBasePos + glm::vec3(0.0f, movementSettings().eyeHeight, 0.0f);
    const glm::vec3 requestPos(req.posX, req.posY, req.posZ);

    const auto computeValidatedOrigin = [&]() -> std::pair<glm::vec3, bool> {
        const glm::vec3 eyeToRequest = requestPos - shooterEyePos;
        const float eyeToRequestDist = glm::length(eyeToRequest);
        bool allowRequestedOrigin = false;
        if (std::isfinite(eyeToRequestDist) && eyeToRequestDist <= kShootOriginTolerance) {
            if (eyeToRequestDist <= 1e-4f) {
                allowRequestedOrigin = true;
            }
            else {
                float eyePathHitDistance = 0.0f;
                glm::vec3 eyePathHitPoint(0.0f);
                const bool eyePathBlocked = FindFirstSolidBlockHit(
                    m_chunkManager,
                    shooterEyePos,
                    eyeToRequest,
                    eyeToRequestDist,
                    eyePathHitDistance,
                    eyePathHitPoint
                );
                allowRequestedOrigin = !eyePathBlocked ||
                    ((eyePathHitDistance + kShootOriginOcclusionEpsilon) >= eyeToRequestDist);
            }
        }
        return { allowRequestedOrigin ? requestPos : shooterEyePos, allowRequestedOrigin };
    };

    const auto [rayOrigin, allowRequestedOrigin] = computeValidatedOrigin();
    const float maxDistance = kShootMaxDistance;
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] shooter=" << session.playerId
            << " lagCompTick=" << (lagCompFrame ? static_cast<int64_t>(lagCompFrame->serverTick) : -1)
            << " origin=(" << rayOrigin.x << "," << rayOrigin.y << "," << rayOrigin.z << ")"
            << " requestedOriginAccepted=" << (allowRequestedOrigin ? "yes" : "no")
            << " maxDistance=" << maxDistance
            << "\n";
    }

    struct HitDetectionResult {
        bool playerHit = false;
        PlayerID hitPlayerId = 0;
        HitRegion hitRegion = HitRegion::Unknown;
        glm::vec3 hitPoint{ 0.0f };
        float bestPlayerDistance = 0.0f;
        bool blockHit = false;
        float blockDistance = 0.0f;
        glm::vec3 blockHitPoint{ 0.0f };
    };

    // Step 5: run hit detection.
    const auto runHitDetection = [&]() -> HitDetectionResult {
        HitDetectionResult detection{};
        detection.hitPoint = rayOrigin + rayDir * maxDistance;
        detection.bestPlayerDistance = maxDistance + 1.0f;

        float meshReferenceHeight = 0.0f;
        MeshBroadphaseBounds meshBroadphase;
        const std::vector<MeshHitTriangle>& meshTriangles =
            GetSharedPlayerMeshHitTriangles(meshReferenceHeight, &meshBroadphase);

        float referenceHeight = 0.0f;
        float referenceRadius = 0.0f;
        const std::vector<Hitbox>* baseHitboxes = nullptr;
        if (meshTriangles.empty()) {
            baseHitboxes = &GetSharedPlayerHitboxes(referenceHeight, referenceRadius);
        }

        const std::vector<ServerPlayerCombatSnapshot> players =
            m_playerManager.getAllCombatSnapshotsCopy(true);
        for (const ServerPlayerCombatSnapshot& target : players) {
            if (target.id == session.playerId) {
                continue;
            }

            glm::vec3 targetPosition = target.position;
            float targetYaw = target.yaw;
            float targetHeight = target.height;
            float targetRadius = target.radius;
            if (lagCompFrame != nullptr) {
                auto targetLagIt = lagCompFrame->players.find(target.id);
                if (targetLagIt != lagCompFrame->players.end()) {
                    const LagCompPlayerPose& pose = targetLagIt->second;
                    targetPosition = pose.position;
                    targetYaw = pose.yaw;
                    targetHeight = pose.height;
                    targetRadius = pose.radius;
                }
            }

            const CachedTransform targetTransform = BuildPlayerTransform(targetPosition, targetYaw);
            glm::vec3 broadphaseCenterWorld(0.0f);
            float broadphaseRadiusWorld = 0.0f;

            if (!meshTriangles.empty() && meshBroadphase.valid) {
                const float uniformScale = (meshReferenceHeight > 1e-4f) ? (targetHeight / meshReferenceHeight) : 1.0f;
                const glm::vec3 meshCenterLocal = meshBroadphase.centerLocal * uniformScale;
                broadphaseCenterWorld = glm::vec3(targetTransform.model * glm::vec4(meshCenterLocal, 1.0f));
                broadphaseRadiusWorld = meshBroadphase.radiusLocal * uniformScale + kShootHitboxPadXZ;
            }
            else {
                const float halfHeight = std::max(0.5f * targetHeight, 1e-3f);
                const float radius = std::max(targetRadius, 1e-3f) + kShootHitboxPadXZ;
                broadphaseCenterWorld = targetPosition + glm::vec3(0.0f, halfHeight, 0.0f);
                broadphaseRadiusWorld = std::sqrt(radius * radius + halfHeight * halfHeight) + kShootHitboxPadY;
            }

            float broadphaseT = 0.0f;
            if (!RayIntersectsSphere(
                    rayOrigin,
                    rayDir,
                    broadphaseCenterWorld,
                    broadphaseRadiusWorld,
                    maxDistance,
                    broadphaseT
                ) ||
                broadphaseT > detection.bestPlayerDistance) {
                continue;
            }

            HitResult hit;
            if (!meshTriangles.empty()) {
                const float uniformScale = (meshReferenceHeight > 1e-4f) ? (targetHeight / meshReferenceHeight) : 1.0f;
                hit = RaycastMeshTriangles(
                    rayOrigin,
                    rayDir,
                    meshTriangles,
                    targetTransform,
                    uniformScale,
                    maxDistance
                );
            }
            else {
                const float sx = (referenceRadius > 1e-4f) ? (targetRadius / referenceRadius) : 1.0f;
                const float sy = (referenceHeight > 1e-4f) ? (targetHeight / referenceHeight) : 1.0f;
                const float sz = sx;

                std::vector<Hitbox> scaledHitboxes;
                scaledHitboxes.reserve(baseHitboxes->size());
                for (const Hitbox& base : *baseHitboxes) {
                    Hitbox scaled = base;
                    scaled.min = glm::vec3(
                        base.min.x * sx - kShootHitboxPadXZ,
                        base.min.y * sy - kShootHitboxPadY,
                        base.min.z * sz - kShootHitboxPadXZ
                    );
                    scaled.max = glm::vec3(
                        base.max.x * sx + kShootHitboxPadXZ,
                        base.max.y * sy + kShootHitboxPadY,
                        base.max.z * sz + kShootHitboxPadXZ
                    );
                    scaledHitboxes.push_back(scaled);
                }

                hit = HitboxManager::raycastHitboxes(
                    rayOrigin,
                    rayDir,
                    scaledHitboxes,
                    targetTransform.model,
                    maxDistance
                );
            }
            const HitRegion resolvedRegion = hit.hit
                ? ResolveHitRegionByPoint(hit.hitPointWorld, targetTransform.invModel, targetHeight, hit.region)
                : HitRegion::Unknown;
            if (kEnableShootValidationLogs && hit.hit) {
                std::cout
                    << "[shoot/validate] candidate player=" << target.id
                    << " dist=" << hit.distance
                    << " region=" << CombatRules::HitRegionName(resolvedRegion)
                    << " hit=(" << hit.hitPointWorld.x << "," << hit.hitPointWorld.y << "," << hit.hitPointWorld.z << ")"
                    << "\n";
            }
            if (hit.hit && hit.distance < detection.bestPlayerDistance) {
                detection.playerHit = true;
                detection.hitPlayerId = target.id;
                detection.hitRegion = resolvedRegion;
                detection.hitPoint = hit.hitPointWorld;
                detection.bestPlayerDistance = hit.distance;
            }
        }

        detection.blockDistance = maxDistance + 1.0f;
        detection.blockHit = FindFirstSolidBlockHit(
            m_chunkManager,
            rayOrigin,
            rayDir,
            maxDistance,
            detection.blockDistance,
            detection.blockHitPoint
        );
        return detection;
    };

    const HitDetectionResult hit = runHitDetection();
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] nearest playerHit=" << (hit.playerHit ? "yes" : "no")
            << " playerDist=" << (hit.playerHit ? hit.bestPlayerDistance : -1.0f)
            << " blockHit=" << (hit.blockHit ? "yes" : "no")
            << " blockDist=" << (hit.blockHit ? hit.blockDistance : -1.0f)
            << "\n";
    }

    // Step 6: resolve shot outcome.
    res.accepted = 1;
    if (!hit.playerHit || (hit.blockHit && (hit.blockDistance + kShootBlockOcclusionEpsilon) <= hit.bestPlayerDistance)) {
        res.didHit = 0;
        const glm::vec3 endpoint = hit.blockHit ? hit.blockHitPoint : (rayOrigin + rayDir * maxDistance);
        res.hitX = endpoint.x;
        res.hitY = endpoint.y;
        res.hitZ = endpoint.z;
        if (kEnableShootValidationLogs) {
            const char* reason = (!hit.playerHit) ? "no_player_intersection" : "occluded_by_block";
            std::cout
                << "[shoot/validate] result=miss reason=" << reason
                << " blockDist=" << (hit.blockHit ? hit.blockDistance : -1.0f)
                << " playerDist=" << (hit.playerHit ? hit.bestPlayerDistance : -1.0f)
                << " epsilon=" << kShootBlockOcclusionEpsilon
                << " endpoint=(" << endpoint.x << "," << endpoint.y << "," << endpoint.z << ")"
                << "\n";
        }
        sendResult(res);
        return;
    }

    float damage = 0;
    switch (hit.hitRegion) {
    case HitRegion::Head:
        damage = CombatRules::HeadshotDamageForWeapon(req.weaponId);
        break;
    case HitRegion::Body:
        damage = CombatRules::TorsoshotDamageForWeapon(req.weaponId);
        break;
    case HitRegion::Legs:
        damage = CombatRules::LegshotDamageForWeapon(req.weaponId);
        break;
    case HitRegion::Unknown:
    default:
        // Unknown region should still apply deterministic damage.
        // Use torso as neutral fallback.
        damage = CombatRules::TorsoshotDamageForWeapon(req.weaponId);
        break;
    }

    float healthAfter = 0.0f;
    bool killed = false;
    if (!m_playerManager.applyDamage(hit.hitPlayerId, damage, healthAfter, killed)) {
        res.didHit = 0;
        res.hitX = hit.hitPoint.x;
        res.hitY = hit.hitPoint.y;
        res.hitZ = hit.hitPoint.z;
        if (kEnableShootValidationLogs) {
            std::cout
                << "[shoot/validate] result=miss reason=apply_damage_failed"
                << " target=" << hit.hitPlayerId
                << "\n";
        }
        sendResult(res);
        return;
    }

    res.didHit = 1;
    res.hitEntityId = (hit.hitPlayerId <= static_cast<PlayerID>(std::numeric_limits<int32_t>::max()))
        ? static_cast<int32_t>(hit.hitPlayerId)
        : -1;
    res.hitX = hit.hitPoint.x;
    res.hitY = hit.hitPoint.y;
    res.hitZ = hit.hitPoint.z;
    res.damageApplied = damage;
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] result=hit"
            << " target=" << hit.hitPlayerId
            << " region=" << CombatRules::HitRegionName(hit.hitRegion)
            << " damage=" << damage
            << " healthAfter=" << healthAfter
            << " killed=" << (killed ? "yes" : "no")
            << " point=(" << hit.hitPoint.x << "," << hit.hitPoint.y << "," << hit.hitPoint.z << ")"
            << "\n";
    }

    // Step 7: apply combat side-effects (score + killfeed).
    if (killed) {
        std::string victimUsername;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_matchEnded) {
                auto killerIt = m_matchScores.find(session.playerId);
                if (killerIt != m_matchScores.end()) {
                    ++killerIt->second.kills;
                }
                auto victimIt = m_matchScores.find(hit.hitPlayerId);
                if (victimIt != m_matchScores.end()) {
                    ++victimIt->second.deaths;
                }
            }
            for (const auto& [_, clientSession] : m_clients) {
                if (clientSession.playerId == hit.hitPlayerId) {
                    victimUsername = clientSession.username;
                    break;
                }
            }
        }
        if (victimUsername.empty()) {
            victimUsername = std::string("Player") + std::to_string(hit.hitPlayerId);
        }

        std::string killPayload = "KILLFEED|";
        killPayload += session.username;
        killPayload += "|";
        killPayload += victimUsername;
        killPayload += "|";
        killPayload += std::to_string(req.weaponId);

        std::string out;
        out.reserve(1 + killPayload.size());
        out.push_back(static_cast<char>(PacketType::Message));
        out += killPayload;
        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
    }
    (void)healthAfter;
    (void)killed;
    sendResult(res);
}
