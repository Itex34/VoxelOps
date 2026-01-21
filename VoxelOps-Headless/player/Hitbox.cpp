#include "Hitbox.hpp"

#include <glm/gtc/matrix_inverse.hpp> // for glm::inverse
#include <algorithm>
#include <limits>
#include <cmath>

// Small epsilon to avoid self-intersection / numerical issues
static constexpr float RAY_EPSILON = 1e-5f;

std::vector<Hitbox> HitboxManager::buildBlockyHitboxes(
    float playerHeight,
    float halfWidth,
    float halfDepth,
    bool originAtFeet)
{
    std::vector<Hitbox> boxes;
    const float eps = 0.01f; // small expansion to avoid seam misses

    // Proportions tuned for a blocky / minecraft-like player
    const float headH = 0.25f;
    const float torsoH = 0.65f;
    const float legsH = playerHeight - headH - torsoH;

    // vertical ranges assuming origin at feet
    float legsMin = 0.0f;
    float legsMax = legsMin + legsH;

    float torsoMin = legsMax;
    float torsoMax = torsoMin + torsoH;

    float headMin = torsoMax;
    float headMax = headMin + headH; // should equal playerHeight

    // If origin is not at feet, shift vertically
    float yShift = originAtFeet ? 0.0f : -playerHeight * 0.5f;

    // HEAD
    boxes.push_back(Hitbox{
        /*min*/ glm::vec3(-halfWidth + eps, headMin + yShift - eps, -halfDepth + eps),
        /*max*/ glm::vec3(halfWidth - eps, headMax + yShift + eps,  halfDepth - eps),
        /*region*/ HitRegion::Head
        });

    // BODY (torso)
    boxes.push_back(Hitbox{
        /*min*/ glm::vec3(-halfWidth + eps, torsoMin + yShift - eps, -halfDepth + eps),
        /*max*/ glm::vec3(halfWidth - eps, torsoMax + yShift + eps,  halfDepth - eps),
        /*region*/ HitRegion::Body
        });

    // LEGS (both legs combined)
    boxes.push_back(Hitbox{
        /*min*/ glm::vec3(-halfWidth + eps, legsMin + yShift - eps, -halfDepth + eps),
        /*max*/ glm::vec3(halfWidth - eps, legsMax + yShift + eps,  halfDepth - eps),
        /*region*/ HitRegion::Legs
        });

    return boxes;
}

/*
 * Ray vs AABB (box) intersection.
 *
 * We transform the ray into model-local space (using invModel), perform a robust
 * slab test against box.min / box.max and, if hit, return the world-space hit
 * point and world-space distance from ray origin to hit.
 *
 * Returns true if intersection occurs within positive t (>= RAY_EPSILON).
 */
bool HitboxManager::rayIntersectsAABB(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDir,
    const Hitbox& box,
    const glm::mat4& modelMatrix,
    glm::vec3& outHitPointWorld,
    float& outDistance)
{
    // Compute inverse model to bring ray into local space
    glm::mat4 invModel = glm::inverse(modelMatrix);

    // Ray origin in local space (w = 1)
    glm::vec3 originLocal = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));

    // Ray direction in local space (w = 0) and normalized
    glm::vec3 dirLocalUnnorm = glm::vec3(invModel * glm::vec4(rayDir, 0.0f));
    float dirLocalLen = glm::length(dirLocalUnnorm);
    if (dirLocalLen <= 1e-9f) {
        // Degenerate direction after transform (shouldn't normally happen)
        return false;
    }
    glm::vec3 dirLocal = dirLocalUnnorm / dirLocalLen;

    // Slab method in local space
    float tmin = -std::numeric_limits<float>::infinity();
    float tmax = std::numeric_limits<float>::infinity();

    for (int i = 0; i < 3; ++i) {
        float o = originLocal[i];
        float d = dirLocal[i];
        float bmin = box.min[i];
        float bmax = box.max[i];

        if (std::abs(d) < 1e-8f) {
            // Ray parallel to slab. If origin not within slab -> miss.
            if (o < bmin || o > bmax) {
                return false;
            }
            // otherwise this axis gives no constraints
        }
        else {
            float invD = 1.0f / d;
            float t1 = (bmin - o) * invD;
            float t2 = (bmax - o) * invD;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmax < tmin) return false; // missed
        }
    }

    // Choose intersection t: prefer entry (tmin), but if origin inside box, use tmax (exit)
    float tLocalHit = (tmin >= RAY_EPSILON) ? tmin : ((tmax >= RAY_EPSILON) ? tmax : -1.0f);
    if (tLocalHit < 0.0f) return false;

    // Compute local hit point
    glm::vec3 hitLocal = originLocal + dirLocal * tLocalHit;

    // Transform hit point back to world space
    glm::vec3 hitWorld = glm::vec3(modelMatrix * glm::vec4(hitLocal, 1.0f));

    // Compute true world-space distance along original ray direction
    // We assume rayDir is not necessarily normalized: compute distance using world positions
    outDistance = glm::length(hitWorld - rayOrigin);
    outHitPointWorld = hitWorld;
    return true;
}

/*
 * Raycast a set of hitboxes and return closest hit (if any).
 * rayDir is expected to be a direction (doesn't have to be normalized).
 */
HitResult HitboxManager::raycastHitboxes(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDir,
    const std::vector<Hitbox>& hitboxes,
    const glm::mat4& modelMatrix,
    float maxDistance)
{
    HitResult result;
    result.hit = false;
    result.region = HitRegion::Unknown;
    result.distance = maxDistance;

    // Iterate all hitboxes and keep the nearest valid hit
    for (const auto& hb : hitboxes) {
        glm::vec3 hitPointWorld;
        float hitDist = 0.0f;
        if (rayIntersectsAABB(rayOrigin, rayDir, hb, modelMatrix, hitPointWorld, hitDist)) {
            if (hitDist <= maxDistance && (!result.hit || hitDist < result.distance)) {
                result.hit = true;
                result.region = hb.region;
                result.hitPointWorld = hitPointWorld;
                result.distance = hitDist;
            }
        }
    }

    return result;
}
