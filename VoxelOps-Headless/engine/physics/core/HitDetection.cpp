#include "HitDetection.hpp"

#include "../../../network/gameplay/Rules.hpp"
#include "../../world/ChunkManager.hpp"
#include "../../world/WorldRaycast.hpp"
#include "../../../../Shared/player/HitboxCache.hpp"
#include "../../../../Shared/player/MeshHitCache.hpp"
#include "../../../../Shared/player/PlayerData.hpp"
#include "../../../../Shared/runtime/Paths.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>

namespace {

constexpr bool kEnableHitboxDiagnostics = false;
constexpr float kHitDetectionEarlyOutDistance = 0.05f;

const std::string &SharedHitboxCachePath() {
    static const std::string kPath =
        Shared::RuntimePaths::ResolveSharedPath("generated/player_hitboxes.bin").generic_string();
    return kPath;
}

const std::string &SharedMeshHitCachePath() {
    static const std::string kPath =
        Shared::RuntimePaths::ResolveSharedPath("generated/player_mesh_hit.bin").generic_string();
    return kPath;
}

inline const Shared::PlayerData::MovementSettings &movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}

struct MeshHitTriangle {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    glm::vec3 c{0.0f};
    HitRegion region = HitRegion::Body;
};

struct CachedTransform {
    glm::mat4 model{1.0f};
    glm::mat4 invModel{1.0f};
};

struct MeshBroadphaseBounds {
    glm::vec3 centerLocal{0.0f};
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

HitRegion ResolveHitRegionByPoint(const glm::vec3 &hitPointWorld, const glm::mat4 &invModelMatrix,
                                  float height, HitRegion fallback) {
    const glm::vec3 hitPointLocal = glm::vec3(invModelMatrix * glm::vec4(hitPointWorld, 1.0f));
    const HitRegion byHeight = RegionFromLocalHeight(hitPointLocal.y, height);
    return (byHeight == HitRegion::Unknown) ? fallback : byHeight;
}

const std::vector<Hitbox> &GetSharedPlayerHitboxes(float &outReferenceHeight,
                                                   float &outReferenceRadius) {
    static std::once_flag initFlag;
    static float referenceHeight = movementSettings().collisionHeight;
    static float referenceRadius = movementSettings().collisionRadius;
    static std::vector<Hitbox> cachedHitboxes;

    std::call_once(initFlag, [&]() {
        float loadedHeight = 0.0f;
        float loadedRadius = 0.0f;
        std::vector<Shared::HitboxCache::Record> records;
        if (Shared::HitboxCache::Load(SharedHitboxCachePath(), loadedHeight, loadedRadius,
                                      records) &&
            !records.empty()) {
            cachedHitboxes.reserve(records.size());
            for (const Shared::HitboxCache::Record &rec : records) {
                Hitbox hb;
                hb.min = glm::vec3(rec.minX, rec.minY, rec.minZ);
                hb.max = glm::vec3(rec.maxX, rec.maxY, rec.maxZ);
                hb.region = Rules::RegionFromCacheCode(rec.region);
                cachedHitboxes.push_back(hb);
            }
            referenceHeight =
                (loadedHeight > 1e-4f) ? loadedHeight : movementSettings().collisionHeight;
            referenceRadius =
                (loadedRadius > 1e-4f) ? loadedRadius : movementSettings().collisionRadius;
            std::cout << "[hitbox] loaded shared cache: " << SharedHitboxCachePath() << "\n";
            if (kEnableHitboxDiagnostics) {
                std::cout << "[hitbox/server] source=cache count=" << cachedHitboxes.size()
                          << " refHeight=" << referenceHeight << " refRadius=" << referenceRadius
                          << "\n";
            }
        }

        if (cachedHitboxes.empty()) {
            cachedHitboxes = HitboxManager::buildBlockyHitboxes(
                movementSettings().collisionHeight, movementSettings().collisionRadius,
                movementSettings().collisionRadius, true);
            std::cerr << "[hitbox] cache missing/unusable, using fallback procedural hitboxes.\n";
            if (kEnableHitboxDiagnostics) {
                std::cout << "[hitbox/server] source=fallback count=" << cachedHitboxes.size()
                          << " refHeight=" << referenceHeight << " refRadius=" << referenceRadius
                          << "\n";
            }
        }
    });

    outReferenceHeight = referenceHeight;
    outReferenceRadius = referenceRadius;
    return cachedHitboxes;
}

const std::vector<MeshHitTriangle> &
GetSharedPlayerMeshHitTriangles(float &outReferenceHeight,
                                MeshBroadphaseBounds *outBroadphaseBounds) {
    static std::once_flag initFlag;
    static float referenceHeight = movementSettings().collisionHeight;
    static std::vector<MeshHitTriangle> cachedTriangles;
    static MeshBroadphaseBounds cachedBounds;

    std::call_once(initFlag, [&]() {
        float loadedHeight = 0.0f;
        std::vector<Shared::MeshHitCache::TriangleRecord> records;
        if (Shared::MeshHitCache::Load(SharedMeshHitCachePath(), loadedHeight, records) &&
            !records.empty()) {
            cachedTriangles.reserve(records.size());
            for (const Shared::MeshHitCache::TriangleRecord &rec : records) {
                MeshHitTriangle tri;
                tri.a = glm::vec3(rec.ax, rec.ay, rec.az);
                tri.b = glm::vec3(rec.bx, rec.by, rec.bz);
                tri.c = glm::vec3(rec.cx, rec.cy, rec.cz);
                tri.region = Rules::RegionFromCacheCode(rec.region);
                cachedTriangles.push_back(tri);
            }
            if (loadedHeight > 1e-4f) {
                referenceHeight = loadedHeight;
            }

            glm::vec3 minP(std::numeric_limits<float>::max());
            glm::vec3 maxP(std::numeric_limits<float>::lowest());
            for (const MeshHitTriangle &tri : cachedTriangles) {
                minP = glm::min(minP, tri.a);
                minP = glm::min(minP, tri.b);
                minP = glm::min(minP, tri.c);
                maxP = glm::max(maxP, tri.a);
                maxP = glm::max(maxP, tri.b);
                maxP = glm::max(maxP, tri.c);
            }

            cachedBounds.centerLocal = 0.5f * (minP + maxP);
            float radiusSq = 0.0f;
            for (const MeshHitTriangle &tri : cachedTriangles) {
                const glm::vec3 da = tri.a - cachedBounds.centerLocal;
                const glm::vec3 db = tri.b - cachedBounds.centerLocal;
                const glm::vec3 dc = tri.c - cachedBounds.centerLocal;
                radiusSq = std::max(radiusSq, glm::dot(da, da));
                radiusSq = std::max(radiusSq, glm::dot(db, db));
                radiusSq = std::max(radiusSq, glm::dot(dc, dc));
            }
            cachedBounds.radiusLocal = std::sqrt(radiusSq);
            cachedBounds.valid =
                std::isfinite(cachedBounds.radiusLocal) && cachedBounds.radiusLocal > 1e-4f;

            if (kEnableHitboxDiagnostics) {
                std::cout << "[hitbox/server] mesh_source=cache triangles="
                          << cachedTriangles.size() << " refHeight=" << referenceHeight
                          << " broadphaseRadius=" << cachedBounds.radiusLocal
                          << " path=" << SharedMeshHitCachePath() << "\n";
            }
        } else if (kEnableHitboxDiagnostics) {
            std::cout << "[hitbox/server] mesh_source=missing path=" << SharedMeshHitCachePath()
                      << " (falling back to AABB hitboxes)\n";
        }
    });

    outReferenceHeight = referenceHeight;
    if (outBroadphaseBounds != nullptr) {
        *outBroadphaseBounds = cachedBounds;
    }
    return cachedTriangles;
}

bool RayIntersectsTriangle(const glm::vec3 &origin,
                           const glm::vec3 &dir,
                           const glm::vec3 &a,
                           const glm::vec3 &b,
                           const glm::vec3 &c,
                           float &outT) {
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

bool RayIntersectsSphere(const glm::vec3 &origin,
                         const glm::vec3 &dirNorm,
                         const glm::vec3 &center,
                         float radius,
                         float maxDistance,
                         float &outNearT) {
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

HitResult RaycastMeshTriangles(const glm::vec3 &rayOrigin,
                               const glm::vec3 &rayDir,
                               const std::vector<MeshHitTriangle> &triangles,
                               const CachedTransform &transform,
                               float uniformScale,
                               float maxDistance) {
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

    for (const MeshHitTriangle &tri : triangles) {
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

CachedTransform BuildPlayerTransform(const glm::vec3 &position, float yaw) {
    CachedTransform transform;
    transform.model = glm::translate(glm::mat4(1.0f), position);
    transform.model =
        glm::rotate(transform.model, glm::radians(Rules::ToModelYawDegrees(yaw)),
                    glm::vec3(0.0f, 1.0f, 0.0f));
    transform.invModel = glm::affineInverse(transform.model);
    return transform;
}

} // namespace

namespace HitDetection {

HitDetectionResult RaycastPlayersAndWorld(const HitDetectionInput &input) {
    HitDetectionResult detection{};
    detection.maxDistance = input.maxDistance;
    detection.hitPoint = input.rayOrigin + input.rayDir * input.maxDistance;
    detection.bestPlayerDistance = input.maxDistance + 1.0f;

    if (input.players == nullptr || input.chunkManager == nullptr) {
        return detection;
    }

    float meshReferenceHeight = 0.0f;
    MeshBroadphaseBounds meshBroadphase;
    const std::vector<MeshHitTriangle> &meshTriangles =
        GetSharedPlayerMeshHitTriangles(meshReferenceHeight, &meshBroadphase);

    float referenceHeight = 0.0f;
    float referenceRadius = 0.0f;
    const std::vector<Hitbox> *baseHitboxes = nullptr;
    if (meshTriangles.empty()) {
        baseHitboxes = &GetSharedPlayerHitboxes(referenceHeight, referenceRadius);
    }

    for (const ServerPlayerCombatSnapshot &target : *input.players) {
        if (target.id == input.shooterId) {
            continue;
        }

        glm::vec3 targetPosition = target.position;
        float targetYaw = target.yaw;
        float targetHeight = target.height;
        float targetRadius = target.radius;
        if (input.lagCompFrame != nullptr) {
            const auto targetLagIt = input.lagCompFrame->players.find(target.id);
            if (targetLagIt != input.lagCompFrame->players.end()) {
                const LagCompensation::LagCompPlayerPose &pose = targetLagIt->second;
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
            const float uniformScale =
                (meshReferenceHeight > 1e-4f) ? (targetHeight / meshReferenceHeight) : 1.0f;
            const glm::vec3 meshCenterLocal = meshBroadphase.centerLocal * uniformScale;
            broadphaseCenterWorld = glm::vec3(targetTransform.model * glm::vec4(meshCenterLocal, 1.0f));
            broadphaseRadiusWorld = meshBroadphase.radiusLocal * uniformScale + input.hitboxPadXZ;
        } else {
            const float halfHeight = std::max(0.5f * targetHeight, 1e-3f);
            const float radius = std::max(targetRadius, 1e-3f) + input.hitboxPadXZ;
            broadphaseCenterWorld = targetPosition + glm::vec3(0.0f, halfHeight, 0.0f);
            broadphaseRadiusWorld =
                std::sqrt(radius * radius + halfHeight * halfHeight) + input.hitboxPadY;
        }

        float broadphaseT = 0.0f;
        if (!RayIntersectsSphere(input.rayOrigin, input.rayDir, broadphaseCenterWorld,
                                 broadphaseRadiusWorld, input.maxDistance, broadphaseT) ||
            broadphaseT > detection.bestPlayerDistance) {
            continue;
        }

        HitResult hit;
        if (!meshTriangles.empty()) {
            const float uniformScale =
                (meshReferenceHeight > 1e-4f) ? (targetHeight / meshReferenceHeight) : 1.0f;
            hit = RaycastMeshTriangles(input.rayOrigin, input.rayDir, meshTriangles, targetTransform,
                                       uniformScale, input.maxDistance);
        } else {
            const float sx = (referenceRadius > 1e-4f) ? (targetRadius / referenceRadius) : 1.0f;
            const float sy = (referenceHeight > 1e-4f) ? (targetHeight / referenceHeight) : 1.0f;
            const float sz = sx;

            std::vector<Hitbox> scaledHitboxes;
            scaledHitboxes.reserve(baseHitboxes->size());
            for (const Hitbox &base : *baseHitboxes) {
                Hitbox scaled = base;
                scaled.min = glm::vec3(base.min.x * sx - input.hitboxPadXZ,
                                       base.min.y * sy - input.hitboxPadY,
                                       base.min.z * sz - input.hitboxPadXZ);
                scaled.max = glm::vec3(base.max.x * sx + input.hitboxPadXZ,
                                       base.max.y * sy + input.hitboxPadY,
                                       base.max.z * sz + input.hitboxPadXZ);
                scaledHitboxes.push_back(scaled);
            }

            hit = HitboxManager::raycastHitboxes(input.rayOrigin, input.rayDir, scaledHitboxes,
                                                 targetTransform.model, input.maxDistance);
        }

        const HitRegion resolvedRegion =
            hit.hit ? ResolveHitRegionByPoint(hit.hitPointWorld, targetTransform.invModel,
                                              targetHeight, hit.region)
                    : HitRegion::Unknown;
        if (input.enableValidationLogs && hit.hit) {
            std::cout << "[shoot/validate] candidate player=" << target.id << " dist=" << hit.distance
                      << " region=" << Rules::HitRegionName(resolvedRegion) << " hit=("
                      << hit.hitPointWorld.x << "," << hit.hitPointWorld.y << ","
                      << hit.hitPointWorld.z << ")\n";
        }

        if (hit.hit && hit.distance < detection.bestPlayerDistance) {
            detection.playerHit = true;
            detection.hitPlayerId = target.id;
            detection.hitRegion = resolvedRegion;
            detection.hitPoint = hit.hitPointWorld;
            detection.bestPlayerDistance = hit.distance;
            if (detection.bestPlayerDistance <= kHitDetectionEarlyOutDistance) {
                break;
            }
        }
    }

    detection.blockDistance = input.maxDistance + 1.0f;
    detection.blockHit =
        WorldRaycast::FindFirstSolidBlockHit(*input.chunkManager, input.rayOrigin, input.rayDir,
                                             input.maxDistance, detection.blockDistance,
                                             detection.blockHitPoint);
    return detection;
}

} // namespace HitDetection

