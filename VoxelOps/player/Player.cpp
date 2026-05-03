#include "Player.hpp"

#include "../world/ChunkManager.hpp"
#include "../graphics/ModelGeometry.hpp"
#include "../../Shared/player/HitboxCache.hpp"
#include "../../Shared/player/MeshHitCache.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/runtime/Paths.hpp"


#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
inline const Shared::PlayerData::MovementSettings &movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}
constexpr bool kPlayerModelYawInvert = true;
constexpr float kPlayerModelYawOffsetDeg = 0.0f;

float NormalizeYawDegrees(float yawDegrees) {
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f)
        y -= 360.0f;
    if (y < -180.0f)
        y += 360.0f;
    return y;
}

float ToModelYawDegrees(float lookYawDegrees) {
    const float signedYaw = kPlayerModelYawInvert ? -lookYawDegrees : lookYawDegrees;
    return NormalizeYawDegrees(signedYaw + kPlayerModelYawOffsetDeg);
}

constexpr bool kEnableHitboxDiagnostics = true;

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

const char *HitRegionName(HitRegion region) {
    switch (region) {
    case HitRegion::Head:
        return "Head";
    case HitRegion::Body:
        return "Body";
    case HitRegion::Legs:
        return "Legs";
    default:
        return "Unknown";
    }
}

void LogHitboxes(const char *source, const std::vector<Hitbox> &hitboxes, float playerHeight,
                 float playerRadius) {
    if (!kEnableHitboxDiagnostics) {
        return;
    }
    std::cout << "[hitbox/client] source=" << source << " count=" << hitboxes.size()
              << " collisionHeight=" << playerHeight << " collisionRadius=" << playerRadius << "\n";
    for (size_t i = 0; i < hitboxes.size(); ++i) {
        const Hitbox &hb = hitboxes[i];
        std::cout << "  [" << i << "] region=" << HitRegionName(hb.region) << " min=(" << hb.min.x
                  << "," << hb.min.y << "," << hb.min.z << ")"
                  << " max=(" << hb.max.x << "," << hb.max.y << "," << hb.max.z << ")"
                  << "\n";
    }
}

HitRegion RegionFromCacheCode(uint8_t code) {
    switch (code) {
    case 0:
        return HitRegion::Legs;
    case 1:
        return HitRegion::Body;
    case 2:
        return HitRegion::Head;
    default:
        return HitRegion::Unknown;
    }
}

uint8_t CacheCodeFromRegion(HitRegion region) {
    switch (region) {
    case HitRegion::Legs:
        return 0;
    case HitRegion::Body:
        return 1;
    case HitRegion::Head:
        return 2;
    default:
        return 1;
    }
}

uint8_t CacheCodeFromModelRegion(ModelRegion region) {
    switch (region) {
    case ModelRegion::Legs:
        return 0;
    case ModelRegion::Body:
        return 1;
    case ModelRegion::Head:
        return 2;
    default:
        return 1;
    }
}

std::vector<Hitbox> BuildHitboxesFromModel(const ModelGeometry &model, float playerHeight,
                                           float playerRadius) {
    std::vector<Hitbox> out;
    const float targetHeight = std::max(playerHeight, 0.01f);
    const float modelMinY = model.getLocalMinY();
    const float modelHeight = model.getLocalHeight();
    const float uniformScale = targetHeight / std::max(modelHeight, 1e-4f);
    const float yOffset = -modelMinY * uniformScale;

    // Use full-body X/Z span to avoid per-region side bias (arms) causing torso/head "holes".
    const glm::vec3 localMin = model.getLocalMinBounds();
    const glm::vec3 localMax = model.getLocalMaxBounds();
    float xMin = localMin.x * uniformScale;
    float xMax = localMax.x * uniformScale;
    float zMin = localMin.z * uniformScale;
    float zMax = localMax.z * uniformScale;

    // Keep combat hitboxes wide enough to include protruding limbs (arms).
    xMin -= 0.02f;
    xMax += 0.02f;
    zMin -= 0.02f;
    zMax += 0.02f;

    const float minHalfExtent = std::max(playerRadius * 0.65f, 0.08f);
    xMin = std::min(xMin, -minHalfExtent);
    xMax = std::max(xMax, minHalfExtent);
    zMin = std::min(zMin, -minHalfExtent);
    zMax = std::max(zMax, minHalfExtent);
    if (xMax <= xMin)
        xMax = xMin + 1e-4f;
    if (zMax <= zMin)
        zMax = zMin + 1e-4f;

    float legsTop = targetHeight * 0.40f;
    float headBottom = targetHeight * 0.72f;
    const ModelRegionAabb &legsRegion = model.getLocalRegionAabb(ModelRegion::Legs);
    const ModelRegionAabb &headRegion = model.getLocalRegionAabb(ModelRegion::Head);
    if (legsRegion.valid) {
        const float candidate = legsRegion.max.y * uniformScale + yOffset;
        if (candidate >= targetHeight * 0.25f && candidate <= targetHeight * 0.58f) {
            legsTop = candidate;
        }
    }
    if (headRegion.valid) {
        const float candidate = headRegion.min.y * uniformScale + yOffset;
        if (candidate >= targetHeight * 0.58f && candidate <= targetHeight * 0.84f) {
            headBottom = candidate;
        }
    }

    const float minLegsTop = targetHeight * 0.25f;
    const float maxLegsTop = targetHeight * 0.60f;
    const float minHeadBottom = targetHeight * 0.58f;
    const float maxHeadBottom = targetHeight * 0.84f;
    legsTop = std::clamp(legsTop, minLegsTop, maxLegsTop);
    headBottom = std::clamp(headBottom, minHeadBottom, maxHeadBottom);

    const float minBodyHeight = targetHeight * 0.20f;
    const float minHeadHeight = targetHeight * 0.16f;
    if ((headBottom - legsTop) < minBodyHeight) {
        headBottom = legsTop + minBodyHeight;
    }
    if ((targetHeight - headBottom) < minHeadHeight) {
        headBottom = targetHeight - minHeadHeight;
    }
    headBottom = std::clamp(headBottom, legsTop + 1e-3f, targetHeight - 1e-3f);

    auto pushBox = [&](float y0, float y1, HitRegion region) {
        Hitbox hb;
        hb.min = glm::vec3(xMin, y0, zMin);
        hb.max = glm::vec3(xMax, y1, zMax);
        if (hb.max.y <= hb.min.y)
            hb.max.y = hb.min.y + 1e-4f;
        hb.region = region;
        out.push_back(hb);
    };

    pushBox(0.0f, legsTop, HitRegion::Legs);
    pushBox(legsTop, headBottom, HitRegion::Body);
    pushBox(headBottom, targetHeight, HitRegion::Head);
    return out;
}

std::vector<Shared::MeshHitCache::TriangleRecord>
BuildMeshTrianglesFromModel(const ModelGeometry &model, float playerHeight) {
    std::vector<Shared::MeshHitCache::TriangleRecord> out;
    const float targetHeight = std::max(playerHeight, 0.01f);
    const float modelMinY = model.getLocalMinY();
    const float modelHeight = model.getLocalHeight();
    const float uniformScale = targetHeight / std::max(modelHeight, 1e-4f);
    const glm::vec3 offset(0.0f, -modelMinY * uniformScale, 0.0f);

    const std::vector<ModelLocalTriangle> &tris = model.getLocalTriangles();
    out.reserve(tris.size());
    for (const ModelLocalTriangle &tri : tris) {
        Shared::MeshHitCache::TriangleRecord rec;
        const glm::vec3 a = tri.a * uniformScale + offset;
        const glm::vec3 b = tri.b * uniformScale + offset;
        const glm::vec3 c = tri.c * uniformScale + offset;
        rec.ax = a.x;
        rec.ay = a.y;
        rec.az = a.z;
        rec.bx = b.x;
        rec.by = b.y;
        rec.bz = b.z;
        rec.cx = c.x;
        rec.cy = c.y;
        rec.cz = c.z;
        rec.region = CacheCodeFromModelRegion(tri.region);
        out.push_back(rec);
    }
    return out;
}

bool SaveHitboxCache(const std::vector<Hitbox> &hitboxes, float playerHeight, float playerRadius) {
    std::vector<Shared::HitboxCache::Record> records;
    records.reserve(hitboxes.size());
    for (const Hitbox &hb : hitboxes) {
        Shared::HitboxCache::Record rec;
        rec.minX = hb.min.x;
        rec.minY = hb.min.y;
        rec.minZ = hb.min.z;
        rec.maxX = hb.max.x;
        rec.maxY = hb.max.y;
        rec.maxZ = hb.max.z;
        rec.region = CacheCodeFromRegion(hb.region);
        records.push_back(rec);
    }
    const bool ok =
        Shared::HitboxCache::Save(SharedHitboxCachePath(), playerHeight, playerRadius, records);
    if (kEnableHitboxDiagnostics) {
        std::cout << "[hitbox/client] cache_write path=" << SharedHitboxCachePath()
                  << " status=" << (ok ? "ok" : "failed") << " count=" << records.size() << "\n";
    }
    return ok;
}

bool SaveMeshHitCache(const std::vector<Shared::MeshHitCache::TriangleRecord> &triangles,
                      float playerHeight) {
    const bool ok = Shared::MeshHitCache::Save(SharedMeshHitCachePath(), playerHeight, triangles);
    if (kEnableHitboxDiagnostics) {
        std::cout << "[hitbox/client] mesh_cache_write path=" << SharedMeshHitCachePath()
                  << " status=" << (ok ? "ok" : "failed") << " triangles=" << triangles.size()
                  << "\n";
    }
    return ok;
}

bool LoadHitboxCache(std::vector<Hitbox> &out, float playerHeight, float playerRadius) {
    float referenceHeight = 0.0f;
    float referenceRadius = 0.0f;
    std::vector<Shared::HitboxCache::Record> records;
    if (!Shared::HitboxCache::Load(SharedHitboxCachePath(), referenceHeight, referenceRadius,
                                   records)) {
        return false;
    }
    if (records.empty()) {
        return false;
    }

    const float sx = (referenceRadius > 1e-4f) ? (playerRadius / referenceRadius) : 1.0f;
    const float sy = (referenceHeight > 1e-4f) ? (playerHeight / referenceHeight) : 1.0f;
    const float sz = sx;

    out.clear();
    out.reserve(records.size());
    for (const Shared::HitboxCache::Record &rec : records) {
        Hitbox hb;
        hb.min = glm::vec3(rec.minX * sx, rec.minY * sy, rec.minZ * sz);
        hb.max = glm::vec3(rec.maxX * sx, rec.maxY * sy, rec.maxZ * sz);
        hb.region = RegionFromCacheCode(rec.region);
        out.push_back(hb);
    }
    if (kEnableHitboxDiagnostics) {
        std::cout << "[hitbox/client] cache_load path=" << SharedHitboxCachePath()
                  << " records=" << records.size() << " refHeight=" << referenceHeight
                  << " refRadius=" << referenceRadius << " scale=(" << sx << "," << sy << "," << sz
                  << ")"
                  << "\n";
    }
    return true;
}
} // namespace

Player::Player(const glm::vec3 &startPos, ChunkManager &inChunkManager,
               const std::string &playerModelPath)
    : position(startPos), velocity(0.0f), front(0.0f, 0.0f, -1.0f), currentFov(walkFov),
      camera(startPos), chunkManager(inChunkManager), onGround(false), yaw(-90.0f), pitch(0.0f) {
    const auto &movement = movementSettings();
    moveSpeed = movement.walkSpeed;
    runSpeed = movement.sprintSpeed;
    jumpVelocity = movement.jumpVelocity;
    playerHeight = movement.collisionHeight;
    playerRadius = movement.collisionRadius;
    std::unique_ptr<ModelGeometry> loadedPlayerModel;
    {
        auto modelGeometry = std::make_unique<ModelGeometry>();
        std::string error;
        if (modelGeometry->loadFromFile(playerModelPath, &error)) {
            loadedPlayerModel = std::move(modelGeometry);
        } else {
            std::cerr << "Model load exception: " << error << "\n";
        }
    }

    if (!LoadHitboxCache(m_hitboxes, playerHeight, playerRadius)) {
        if (loadedPlayerModel) {
            m_hitboxes = BuildHitboxesFromModel(*loadedPlayerModel, playerHeight, playerRadius);
            if (!m_hitboxes.empty()) {
                if (!SaveHitboxCache(m_hitboxes, playerHeight, playerRadius)) {
                    std::cerr << "Warning: failed to write hitbox cache: "
                              << SharedHitboxCachePath() << "\n";
                }
                LogHitboxes("mesh_derived", m_hitboxes, playerHeight, playerRadius);
            }
        }
    } else {
        LogHitboxes("cache", m_hitboxes, playerHeight, playerRadius);
    }
    if (m_hitboxes.empty()) {
        m_hitboxes =
            HitboxManager::buildBlockyHitboxes(playerHeight, playerRadius, playerRadius, true);
        LogHitboxes("fallback_blocky", m_hitboxes, playerHeight, playerRadius);
    }

    if (loadedPlayerModel) {
        const auto triangles = BuildMeshTrianglesFromModel(*loadedPlayerModel, playerHeight);
        if (!triangles.empty()) {
            if (!SaveMeshHitCache(triangles, playerHeight)) {
                std::cerr << "Warning: failed to write mesh hit cache: " << SharedMeshHitCachePath()
                          << "\n";
            }
        }
    }

    syncCameraToBody();

    // sanity checks
    if (!loadedPlayerModel)
        std::cerr << "Warning: playerModel not loaded\n";
}

const std::vector<Hitbox> &Player::getHitboxes() const noexcept {
    return m_hitboxes;
}

const glm::mat4 &Player::getModelMatrix() const noexcept {
    return m_modelMatrix;
}

// update the cached model matrix used for hitbox / rendering transforms.
// We translate to the player's position (feet position) and rotate around Y by yaw so
// local hitboxes oriented with player's facing direction are transformed correctly.
void Player::updateModelMatrix() noexcept {
    glm::mat4 model(1.0f);
    // translate to feet
    model = glm::translate(model, position);

    // rotate by yaw so hitboxes follow player facing. yaw is degrees in this class.
    float yawDeg = ToModelYawDegrees(static_cast<float>(yaw));
    model = glm::rotate(model, glm::radians(yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));

    // If you want to offset model so that hitboxes use eye origin, add +vec3(0, eyeOffset, 0)
    // model = glm::translate(model, glm::vec3(0.0f, eyeOffset, 0.0f));

    m_modelMatrix = model;
}

void Player::syncCameraToBody() noexcept {
    const auto &movement = movementSettings();
    camera.position = position + glm::vec3(0.0f, movement.eyeHeight - m_stepUpVisualOffset, 0.0f);
    front = camera.front;
    updateModelMatrix();
}

void Player::decayStepUpOffset(float dt) noexcept {
    if (dt <= 0.0f || m_stepUpVisualOffset <= 0.0f) {
        return;
    }
    const float keep = std::exp(-m_stepUpSmoothingSpeed * dt);
    m_stepUpVisualOffset *= std::clamp(keep, 0.0f, 1.0f);
    if (m_stepUpVisualOffset < 1e-4f) {
        m_stepUpVisualOffset = 0.0f;
    }
}



void Player::setPosition(const glm::vec3 &p) noexcept {
    position = p;
    syncCameraToBody();
}







// update (called each frame)


void Player::setConnectedPlayers(const std::unordered_map<PlayerID, PlayerState> &players) {
    m_remotePlayerTargets.clear();
    m_remotePlayerTargets.reserve(players.size());

    for (const auto &[id, state] : players) {
        auto currentIt = connectedPlayers.find(id);
        if (currentIt == connectedPlayers.end()) {
            connectedPlayers.emplace(id, state);
        }
        m_remotePlayerTargets.emplace(id, state);
    }

    for (auto it = connectedPlayers.begin(); it != connectedPlayers.end();) {
        if (m_remotePlayerTargets.find(it->first) == m_remotePlayerTargets.end()) {
            it = connectedPlayers.erase(it);
        } else {
            ++it;
        }
    }
}

void Player::clearConnectedPlayers() {
    connectedPlayers.clear();
    m_remotePlayerTargets.clear();
}

void Player::updateRemotePlayers(float deltaTime) {
    if (connectedPlayers.empty()) {
        return;
    }

    const float dt = std::max(0.0f, deltaTime);
    const float blend = std::clamp(1.0f - std::exp(-12.0f * dt), 0.0f, 1.0f);

    for (auto &[id, current] : connectedPlayers) {
        auto targetIt = m_remotePlayerTargets.find(id);
        if (targetIt == m_remotePlayerTargets.end()) {
            continue;
        }
        const PlayerState &target = targetIt->second;
        current.position = glm::mix(current.position, target.position, blend);
        current.rotation = glm::normalize(glm::slerp(current.rotation, target.rotation, blend));
        current.scale = glm::mix(current.scale, target.scale, blend);
        current.weaponId = target.weaponId;
    }
}






void Player::breakBlock() {
    Ray ray(camera.position, camera.front);
    if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hit) {
        glm::ivec3 hitBlock = rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hitBlockWorld;

        chunkManager.playerBreakBlockAt(hitBlock);
    }
}
