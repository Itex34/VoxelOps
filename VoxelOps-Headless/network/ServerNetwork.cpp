#include "ServerNetwork.hpp"
#include "../player/Hitbox.hpp"
#include "../../Shared/gun/GunType.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/player/HitboxCache.hpp"
#include "../../Shared/player/MeshHitCache.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/vec3.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>


using namespace std;

ServerNetwork* ServerNetwork::s_instance = nullptr;

namespace {
std::atomic<bool> g_enableChunkDiagnostics{ false };
std::atomic<bool> g_enableServerPerfDiagnostics{ true };
constexpr auto kServerPerfLogInterval = std::chrono::seconds(1);
constexpr double kSlowServerLoopWarnUs = 12000.0;
constexpr double kSlowServerSimWarnUs = 5000.0;
constexpr uint32_t kMaxInboundPacketBytes = 64u * 1024u;
constexpr uint32_t kMaxConnectRequestBytes =
    1u + 2u + 1u + 1u +
    static_cast<uint32_t>(kMaxConnectIdentityChars) +
    static_cast<uint32_t>(kMaxConnectUsernameChars);
constexpr uint32_t kMaxChatMessageBytes = 1u + 512u;
constexpr uint32_t kPlayerInputPacketBytes = 1u + 4u + 1u + 1u + 2u + 4u * 4u;
constexpr uint32_t kPlayerPositionPacketBytes = 1u + 4u + 6u * 4u;
constexpr uint32_t kChunkRequestPacketBytes = 1u + 4u + 4u + 4u + 2u;
constexpr uint32_t kChunkAckPacketBytes = 1u + 1u + 4u + 4u + 4u + 4u + 8u;
constexpr uint32_t kShootRequestPacketBytes = 1u + 4u + 4u + 2u + 12u + 12u + 4u + 1u;
constexpr auto kInboundRateWindow = std::chrono::seconds(1);
constexpr uint32_t kMaxInboundPacketsPerWindow = 900u;
constexpr uint32_t kMaxInboundBytesPerWindow = 256u * 1024u;
constexpr uint32_t kMaxPlayerInputsPerWindow = 360u;
constexpr uint32_t kMaxChunkRequestsPerWindow = 120u;
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
constexpr bool kPlayerModelYawInvert = true;
constexpr float kPlayerModelYawOffsetDeg = 0.0f;

float NormalizeYawDegrees(float yawDegrees)
{
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f) y -= 360.0f;
    if (y < -180.0f) y += 360.0f;
    return y;
}

float ToModelYawDegrees(float lookYawDegrees)
{
    const float signedYaw = kPlayerModelYawInvert ? -lookYawDegrees : lookYawDegrees;
    return NormalizeYawDegrees(signedYaw + kPlayerModelYawOffsetDeg);
}

bool IsValidIdentityChar(char c)
{
    return
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '_' ||
        c == '-';
}

std::string NormalizeIdentity(std::string identity)
{
    std::string out;
    out.reserve(identity.size());
    for (char c : identity) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (IsValidIdentityChar(lower)) {
            out.push_back(lower);
        }
    }
    if (out.size() > kMaxConnectIdentityChars) {
        out.resize(kMaxConnectIdentityChars);
    }
    return out;
}

bool IsValidIdentity(const std::string& identity)
{
    if (identity.empty() || identity.size() > kMaxConnectIdentityChars) {
        return false;
    }
    for (char c : identity) {
        if (!IsValidIdentityChar(c)) {
            return false;
        }
    }
    return true;
}

bool IsValidDisplayNameChar(char c)
{
    return
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' ||
        c == '-';
}

std::string NormalizeDisplayName(std::string name)
{
    size_t begin = 0;
    while (begin < name.size() && std::isspace(static_cast<unsigned char>(name[begin])) != 0) {
        ++begin;
    }
    size_t end = name.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(name[end - 1])) != 0) {
        --end;
    }

    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        const char c = name[i];
        if (IsValidDisplayNameChar(c)) {
            out.push_back(c);
        }
    }
    if (out.size() > kMaxConnectUsernameChars) {
        out.resize(kMaxConnectUsernameChars);
    }
    return out;
}

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

float HeadshotDamageForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol): return 33.0f;
    case ToWeaponId(GunType::Sniper): return 100.0f;
    default: return 33.0f;
    }
}

float TorsoshotDamageForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol): return 25.0f;
    case ToWeaponId(GunType::Sniper): return 75.0f;
    default: return 25.0f;
    }
}

float LegshotDamageForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol): return 18.0f;
    case ToWeaponId(GunType::Sniper): return 50.0f;
    default: return 18.0f;
    }
}


float MinSecondsPerShotForWeapon(uint16_t weaponId) {
    switch (weaponId) {
    case ToWeaponId(GunType::Pistol):
    case ToWeaponId(GunType::Sniper):
    default:
        return kShootMinIntervalSeconds;
    }
}

const char* HitRegionName(HitRegion region) {
    switch (region) {
    case HitRegion::Head: return "Head";
    case HitRegion::Body: return "Body";
    case HitRegion::Legs: return "Legs";
    default: return "Unknown";
    }
}

HitRegion RegionFromCacheCode(uint8_t code) {
    switch (code) {
    case 0: return HitRegion::Legs;
    case 1: return HitRegion::Body;
    case 2: return HitRegion::Head;
    default: return HitRegion::Unknown;
    }
}

struct MeshHitTriangle {
    glm::vec3 a{ 0.0f };
    glm::vec3 b{ 0.0f };
    glm::vec3 c{ 0.0f };
    HitRegion region = HitRegion::Body;
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
    const glm::mat4& modelMatrix,
    float height,
    HitRegion fallback
) {
    const glm::mat4 invModel = glm::inverse(modelMatrix);
    const glm::vec3 hitPointLocal = glm::vec3(invModel * glm::vec4(hitPointWorld, 1.0f));
    const HitRegion byHeight = RegionFromLocalHeight(hitPointLocal.y, height);
    return (byHeight == HitRegion::Unknown) ? fallback : byHeight;
}

const std::vector<Hitbox>& GetSharedPlayerHitboxes(float& outReferenceHeight, float& outReferenceRadius) {
    static bool initialized = false;
    static float referenceHeight = movementSettings().collisionHeight;
    static float referenceRadius = movementSettings().collisionRadius;
    static std::vector<Hitbox> cachedHitboxes;

    if (!initialized) {
        initialized = true;
        float loadedHeight = 0.0f;
        float loadedRadius = 0.0f;
        std::vector<Shared::HitboxCache::Record> records;
        if (Shared::HitboxCache::Load(SharedHitboxCachePath(), loadedHeight, loadedRadius, records) && !records.empty()) {
            cachedHitboxes.reserve(records.size());
            for (const Shared::HitboxCache::Record& rec : records) {
                Hitbox hb;
                hb.min = glm::vec3(rec.minX, rec.minY, rec.minZ);
                hb.max = glm::vec3(rec.maxX, rec.maxY, rec.maxZ);
                hb.region = RegionFromCacheCode(rec.region);
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
                        << "  [" << i << "] region=" << HitRegionName(hb.region)
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
    }

    outReferenceHeight = referenceHeight;
    outReferenceRadius = referenceRadius;
    return cachedHitboxes;
}

const std::vector<MeshHitTriangle>& GetSharedPlayerMeshHitTriangles(float& outReferenceHeight) {
    static bool initialized = false;
    static float referenceHeight = movementSettings().collisionHeight;
    static std::vector<MeshHitTriangle> cachedTriangles;

    if (!initialized) {
        initialized = true;
        float loadedHeight = 0.0f;
        std::vector<Shared::MeshHitCache::TriangleRecord> records;
        if (Shared::MeshHitCache::Load(SharedMeshHitCachePath(), loadedHeight, records) && !records.empty()) {
            cachedTriangles.reserve(records.size());
            for (const Shared::MeshHitCache::TriangleRecord& rec : records) {
                MeshHitTriangle tri;
                tri.a = glm::vec3(rec.ax, rec.ay, rec.az);
                tri.b = glm::vec3(rec.bx, rec.by, rec.bz);
                tri.c = glm::vec3(rec.cx, rec.cy, rec.cz);
                tri.region = RegionFromCacheCode(rec.region);
                cachedTriangles.push_back(tri);
            }
            if (loadedHeight > 1e-4f) {
                referenceHeight = loadedHeight;
            }
            if (kEnableHitboxDiagnostics) {
                std::cout
                    << "[hitbox/server] mesh_source=cache triangles=" << cachedTriangles.size()
                    << " refHeight=" << referenceHeight
                    << " path=" << SharedMeshHitCachePath()
                    << "\n";
            }
        }
        else if (kEnableHitboxDiagnostics) {
            std::cout
                << "[hitbox/server] mesh_source=missing path=" << SharedMeshHitCachePath()
                << " (falling back to AABB hitboxes)\n";
        }
    }

    outReferenceHeight = referenceHeight;
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

HitResult RaycastMeshTriangles(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDir,
    const std::vector<MeshHitTriangle>& triangles,
    const glm::mat4& modelMatrix,
    float uniformScale,
    float maxDistance
) {
    HitResult out;
    out.hit = false;
    out.region = HitRegion::Unknown;
    out.distance = maxDistance;

    const glm::mat4 invModel = glm::inverse(modelMatrix);
    const glm::vec3 originLocal = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
    glm::vec3 dirLocal = glm::vec3(invModel * glm::vec4(rayDir, 0.0f));
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
            out.hitPointWorld = glm::vec3(modelMatrix * glm::vec4(hitLocal, 1.0f));
        }
    }

    return out;
}

glm::mat4 BuildPlayerModelMatrix(const glm::vec3& position, float yaw) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, position);
    model = glm::rotate(model, glm::radians(ToModelYawDegrees(yaw)), glm::vec3(0.0f, 1.0f, 0.0f));
    return model;
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

bool IsInboundPacketSizeValid(PacketType type, uint32_t bytes)
{
    constexpr uint32_t kMinConnectRequestBytes = 1u + 2u + 1u + 1u;
    switch (type) {
    case PacketType::ConnectRequest: return bytes >= kMinConnectRequestBytes && bytes <= kMaxConnectRequestBytes;
    case PacketType::Message: return bytes >= 1u && bytes <= kMaxChatMessageBytes;
    case PacketType::PlayerInput: return bytes == kPlayerInputPacketBytes;
    case PacketType::PlayerPosition: return bytes == kPlayerPositionPacketBytes;
    case PacketType::ChunkRequest: return bytes == kChunkRequestPacketBytes;
    case PacketType::ChunkAck: return bytes == kChunkAckPacketBytes;
    case PacketType::ShootRequest: return bytes == kShootRequestPacketBytes;
    default: return bytes >= 1u && bytes <= kMaxInboundPacketBytes;
    }
}

uint16_t ReadU16LE(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
        (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

int32_t ReadI32LE(const uint8_t* p)
{
    return static_cast<int32_t>(ReadU32LE(p));
}

uint64_t ReadU64LE(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(p[i]) << (8 * i));
    }
    return v;
}

float ReadF32LE(const uint8_t* p)
{
    const uint32_t bits = ReadU32LE(p);
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

bool ParsePlayerInputPacket(const uint8_t* data, uint32_t size, PlayerInput& out)
{
    if (!data || size != kPlayerInputPacketBytes) {
        return false;
    }
    out.sequenceNumber = ReadU32LE(data + 1);
    out.inputFlags = data[5];
    out.flyMode = data[6];
    out.weaponId = ReadU16LE(data + 7);
    out.yaw = ReadF32LE(data + 9);
    out.pitch = ReadF32LE(data + 13);
    out.moveX = ReadF32LE(data + 17);
    out.moveZ = ReadF32LE(data + 21);
    return std::isfinite(out.yaw) &&
        std::isfinite(out.pitch) &&
        std::isfinite(out.moveX) &&
        std::isfinite(out.moveZ);
}

bool ParseChunkRequestPacket(const uint8_t* data, uint32_t size, ChunkRequest& out)
{
    if (!data || size != kChunkRequestPacketBytes) {
        return false;
    }
    out.chunkX = ReadI32LE(data + 1);
    out.chunkY = ReadI32LE(data + 5);
    out.chunkZ = ReadI32LE(data + 9);
    out.viewDistance = ReadU16LE(data + 13);
    return true;
}

bool ParseChunkAckPacket(const uint8_t* data, uint32_t size, ChunkAck& out)
{
    if (!data || size != kChunkAckPacketBytes) {
        return false;
    }
    out.ackedType = data[1];
    out.sequence = ReadU32LE(data + 2);
    out.chunkX = ReadI32LE(data + 6);
    out.chunkY = ReadI32LE(data + 10);
    out.chunkZ = ReadI32LE(data + 14);
    out.version = ReadU64LE(data + 18);
    return true;
}

bool ParseShootRequestPacket(const uint8_t* data, uint32_t size, ShootRequest& out)
{
    if (!data || size != kShootRequestPacketBytes) {
        return false;
    }
    out.clientShotId = ReadU32LE(data + 1);
    out.clientTick = ReadU32LE(data + 5);
    out.weaponId = ReadU16LE(data + 9);
    out.posX = ReadF32LE(data + 11);
    out.posY = ReadF32LE(data + 15);
    out.posZ = ReadF32LE(data + 19);
    out.dirX = ReadF32LE(data + 23);
    out.dirY = ReadF32LE(data + 27);
    out.dirZ = ReadF32LE(data + 31);
    out.seed = ReadU32LE(data + 35);
    out.inputFlags = data[39];
    return
        std::isfinite(out.posX) &&
        std::isfinite(out.posY) &&
        std::isfinite(out.posZ) &&
        std::isfinite(out.dirX) &&
        std::isfinite(out.dirY) &&
        std::isfinite(out.dirZ);
}
}

ServerNetwork::ServerNetwork()
    : m_quit(false),
    m_pollGroup(k_HSteamNetPollGroup_Invalid),
    m_listenSock(k_HSteamListenSocket_Invalid)
{
    // allow only one instance to own the static callback bridge
    s_instance = this;
}

ServerNetwork::~ServerNetwork()
{
    Stop();
    ShutdownNetworking();
    // cleanup pointer
    if (s_instance == this) s_instance = nullptr;
}

bool ServerNetwork::Start(uint16_t port)
{
    if (m_started.load(std::memory_order_acquire)) {
        std::cerr << "ServerNetwork already started\n";
        return false;
    }

    m_quit.store(false, std::memory_order_release);
    m_serverTick.store(0, std::memory_order_release);
    m_lagCompFrames.clear();
    m_matchStartTime = std::chrono::steady_clock::now();
    m_matchStarted = false;
    m_matchEnded = false;
    m_matchWinner.clear();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_matchScores.clear();
    }
    {
        std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
        m_shutdownComplete = false;
    }

    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        std::cerr << "GameNetworkingSockets_Init failed: " << err << "\n";
        return false;
    }

    LoadHistoryFromFile();
    LoadAdminsFromFile();

    // Create poll group (used to efficiently receive messages from many connections)
    m_pollGroup = SteamNetworkingSockets()->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid) {
        std::cerr << "CreatePollGroup failed\n";
        GameNetworkingSockets_Kill();
        return false;
    }

    // Prepare listen socket option to install our connection-status callback
    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(ServerNetwork::SteamNetConnectionStatusChangedCallback));

    // Create listen socket bound to the chosen port
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;
    m_listenSock = SteamNetworkingSockets()->CreateListenSocketIP(addr, 1, &opt);
    if (m_listenSock == k_HSteamListenSocket_Invalid) {
        std::cerr << "CreateListenSocketIP failed\n";
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
        GameNetworkingSockets_Kill();
        return false;
    }

    // Print bound address for debugging
    SteamNetworkingIPAddr boundAddr;
    if (SteamNetworkingSockets()->GetListenSocketAddress(m_listenSock, &boundAddr)) {
        char s[SteamNetworkingIPAddr::k_cchMaxString];
        boundAddr.ToString(s, sizeof(s), true);
        std::cout << "Server listening on " << s << " (Ctrl+C to quit)\n";
    }
    else {
        std::cout << "Server listening on UDP port " << port << " (Ctrl+C to quit)\n";
    }

    StartChunkPipeline();
    m_started.store(true, std::memory_order_release);
    return true;
}

void ServerNetwork::Run()
{
    if (!m_started.load(std::memory_order_acquire)) {
        std::cerr << "ServerNetwork::Run called before Start\n";
        return;
    }
    MainLoop();
    ShutdownNetworking();
}

void ServerNetwork::Stop()
{
    m_quit.store(true, std::memory_order_release);
}

void ServerNetwork::ShutdownNetworking()
{
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
    if (m_shutdownComplete) {
        return;
    }
    m_shutdownComplete = true;

    StopChunkPipeline();
    SaveHistoryToFile();
    SaveAdminsToFile();

    std::vector<std::pair<HSteamNetConnection, ClientSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        sessions.reserve(m_clients.size());
        for (const auto& kv : m_clients) {
            sessions.push_back(kv);
        }
        m_clients.clear();
        m_matchScores.clear();
    }

    for (const auto& [conn, session] : sessions) {
        ClearChunkPipelineForConnection(conn);
        if (session.playerId != 0) {
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_matchScores.erase(session.playerId);
            }
            m_playerManager.removePlayer(session.playerId);
        }
        SteamNetworkingSockets()->CloseConnection(conn, 0, "server shutting down", false);
    }

    if (m_listenSock != k_HSteamListenSocket_Invalid) {
        SteamNetworkingSockets()->CloseListenSocket(m_listenSock);
        m_listenSock = k_HSteamListenSocket_Invalid;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    if (m_started.exchange(false, std::memory_order_acq_rel)) {
        GameNetworkingSockets_Kill();
    }
}

void ServerNetwork::RecordLagCompFrame(uint32_t serverTick)
{
    LagCompFrame frame{};
    frame.serverTick = serverTick;

    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    frame.players.reserve(players.size());
    for (const ServerPlayer& player : players) {
        if (!player.isAlive) {
            continue;
        }
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

static int FloorDiv(int a, int b) {
    int q = a / b;
    const int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

void ServerNetwork::UpdateChunkStreamingForClient(HSteamNetConnection conn, const glm::ivec3& centerChunk, uint16_t viewDistance)
{
    constexpr size_t kMaxChunkPrepQueuePerUpdate = 128;
    constexpr size_t kMaxPendingChunkData = 256;
    constexpr auto kChunkRetryInterval = std::chrono::milliseconds(500);
    const uint16_t clampedViewDistance = ClampViewDistance(viewDistance);
    const auto now = std::chrono::steady_clock::now();

    std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
    const int minChunkY = FloorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = FloorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    const int radius = static_cast<int>(clampedViewDistance);
    const int64_t radius2 = static_cast<int64_t>(radius) * static_cast<int64_t>(radius);
    desired.reserve(static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1) * (maxChunkY - minChunkY + 1)));

    for (int x = centerChunk.x - radius; x <= centerChunk.x + radius; ++x) {
        const int64_t dx = static_cast<int64_t>(x - centerChunk.x);
        const int64_t dx2 = dx * dx;
        for (int z = centerChunk.z - radius; z <= centerChunk.z + radius; ++z) {
            const int64_t dz = static_cast<int64_t>(z - centerChunk.z);
            if (dx2 + dz * dz > radius2) {
                continue;
            }
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                glm::ivec3 pos(x, y, z);
                if (!m_chunkManager.inBounds(pos)) {
                    continue;
                }
                desired.insert(ChunkCoord{ x, y, z });
            }
        }
    }

    std::unordered_set<ChunkCoord, ChunkCoordHash> currentlyStreamed;
    std::unordered_set<ChunkCoord, ChunkCoordHash> pendingPossiblySent;
    std::unordered_map<ChunkCoord, std::chrono::steady_clock::time_point, ChunkCoordHash> pendingChunkData;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(conn);
        if (it == m_clients.end()) {
            return;
        }

        it->second.interestCenterChunk = centerChunk;
        it->second.viewDistance = clampedViewDistance;
        it->second.hasChunkInterest = true;

        currentlyStreamed = it->second.streamedChunks;
        pendingPossiblySent.reserve(it->second.pendingChunkData.size());
        for (const auto& entry : it->second.pendingChunkData) {
            pendingPossiblySent.insert(entry.first);
        }

        for (auto pIt = it->second.pendingChunkData.begin(); pIt != it->second.pendingChunkData.end();) {
            if (desired.find(pIt->first) == desired.end()) {
                it->second.pendingChunkDataPayloadHash.erase(pIt->first);
                pIt = it->second.pendingChunkData.erase(pIt);
            }
            else {
                ++pIt;
            }
        }

        pendingChunkData = it->second.pendingChunkData;
    }

    PruneChunkPipelineForClient(conn, desired);

    std::vector<ChunkCoord> toLoad;
    toLoad.reserve(desired.size());
    for (const ChunkCoord& c : desired) {
        if (currentlyStreamed.find(c) != currentlyStreamed.end()) {
            continue;
        }

        auto pendingIt = pendingChunkData.find(c);
        if (pendingIt != pendingChunkData.end()) {
            if ((now - pendingIt->second) < kChunkRetryInterval) {
                continue;
            }
        }

        toLoad.push_back(c);
    }

    const bool isInitialSync = currentlyStreamed.empty();
    int verticalAnchorY = std::clamp(centerChunk.y, minChunkY, maxChunkY);
    if (verticalAnchorY == maxChunkY && maxChunkY > minChunkY) {
        // Top-most chunk layers are often sparse; bias one layer down to prioritize terrain.
        --verticalAnchorY;
    }
    std::sort(toLoad.begin(), toLoad.end(), [&](const ChunkCoord& a, const ChunkCoord& b) {
        const int adx = a.x - centerChunk.x;
        const int adz = a.z - centerChunk.z;
        const int bdx = b.x - centerChunk.x;
        const int bdz = b.z - centerChunk.z;
        const int aHorizDist2 = adx * adx + adz * adz;
        const int bHorizDist2 = bdx * bdx + bdz * bdz;
        if (aHorizDist2 != bHorizDist2) {
            return aHorizDist2 < bHorizDist2;
        }

        if (isInitialSync) {
            const bool aUnderOrSame = (a.y <= verticalAnchorY);
            const bool bUnderOrSame = (b.y <= verticalAnchorY);
            if (aUnderOrSame != bUnderOrSame) {
                return aUnderOrSame;
            }
        }

        const int aVert = std::abs(a.y - verticalAnchorY);
        const int bVert = std::abs(b.y - verticalAnchorY);
        if (aVert != bVert) {
            return aVert < bVert;
        }

        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });

    std::unordered_set<ChunkCoord, ChunkCoordHash> toUnloadSet;
    toUnloadSet.reserve(currentlyStreamed.size() + pendingPossiblySent.size());
    for (const ChunkCoord& c : currentlyStreamed) {
        if (desired.find(c) == desired.end()) {
            toUnloadSet.insert(c);
        }
    }
    for (const ChunkCoord& c : pendingPossiblySent) {
        if (desired.find(c) == desired.end()) {
            toUnloadSet.insert(c);
        }
    }
    std::vector<ChunkCoord> toUnload;
    toUnload.reserve(toUnloadSet.size());
    for (const ChunkCoord& c : toUnloadSet) {
        toUnload.push_back(c);
    }

    size_t pendingCount = pendingChunkData.size();
    size_t queuedPrepThisUpdate = 0;
    size_t sentThisUpdate = 0;
    bool stoppedByPendingCap = false;
    bool stoppedByPrepCap = false;
    for (const ChunkCoord& c : toLoad) {
        const bool isRetry = pendingChunkData.find(c) != pendingChunkData.end();
        if (queuedPrepThisUpdate >= kMaxChunkPrepQueuePerUpdate) {
            break;
        }
        if (!isRetry && pendingCount >= kMaxPendingChunkData) {
            stoppedByPendingCap = true;
            break;
        }
        if (!QueueChunkPreparation(conn, c)) {
            stoppedByPrepCap = true;
            break;
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(conn);
            if (it != m_clients.end()) {
                // Mark as pending as soon as chunk work is queued to enforce backpressure.
                const bool wasPending = it->second.pendingChunkData.find(c) != it->second.pendingChunkData.end();
                it->second.pendingChunkData[c] = now;
                if (!wasPending) {
                    ++pendingCount;
                }
            }
        }
        ++queuedPrepThisUpdate;
    }

    const size_t sendQueueDepth = GetChunkSendQueueDepthForClient(conn);

    static std::unordered_map<HSteamNetConnection, std::chrono::steady_clock::time_point> s_lastProgressLog;
    auto& lastLog = s_lastProgressLog[conn];
    if (g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        (now - lastLog) >= std::chrono::seconds(1)) {
        lastLog = now;
        std::cerr
            << "[chunk/stream] progress conn=" << conn
            << " desired=" << desired.size()
            << " streamed=" << currentlyStreamed.size()
            << " pending=" << pendingCount
            << " toLoad=" << toLoad.size()
            << " queuedPrepNow=" << queuedPrepThisUpdate
            << " sentNow=" << sentThisUpdate
            << " pendingCapHit=" << (stoppedByPendingCap ? 1 : 0)
            << " prepCapHit=" << (stoppedByPrepCap ? 1 : 0)
            << " sendQueue=" << sendQueueDepth
            << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
            << " viewDist=" << clampedViewDistance << "\n";
    }

    if (g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        !toLoad.empty() && queuedPrepThisUpdate == 0 && sentThisUpdate == 0) {
        std::cerr
            << "[chunk/stream] stalled load window conn=" << conn
            << " desired=" << desired.size()
            << " toLoad=" << toLoad.size()
            << " streamed=" << currentlyStreamed.size()
            << " pending=" << pendingCount
            << " pendingCap=" << kMaxPendingChunkData
            << " prepQueueCap=" << kMaxChunkPrepQueue
            << " sendQueue=" << sendQueueDepth
            << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
            << " viewDist=" << clampedViewDistance << "\n";
    }

    for (const ChunkCoord& c : toUnload) {
        if (!SendChunkUnload(conn, c)) {
            continue;
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(conn);
        if (it != m_clients.end()) {
            it->second.streamedChunks.erase(c);
            it->second.pendingChunkDataPayloadHash.erase(c);
            it->second.pendingChunkData.erase(c);
        }
    }
}

bool ServerNetwork::IsInboundRateLimitExceeded(HSteamNetConnection incoming, PacketType packetType, uint32_t bytes)
{
    bool exceededRateLimit = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            auto& session = it->second;
            const auto now = std::chrono::steady_clock::now();
            if (session.inboundRateWindowStart == std::chrono::steady_clock::time_point::min() ||
                (now - session.inboundRateWindowStart) >= kInboundRateWindow) {
                session.inboundRateWindowStart = now;
                session.inboundPacketsInWindow = 0;
                session.inboundBytesInWindow = 0;
                session.inboundPlayerInputsInWindow = 0;
                session.inboundChunkRequestsInWindow = 0;
            }

            ++session.inboundPacketsInWindow;
            session.inboundBytesInWindow += bytes;
            if (packetType == PacketType::PlayerInput) {
                ++session.inboundPlayerInputsInWindow;
            }
            if (packetType == PacketType::ChunkRequest) {
                ++session.inboundChunkRequestsInWindow;
            }

            exceededRateLimit =
                (session.inboundPacketsInWindow > kMaxInboundPacketsPerWindow) ||
                (session.inboundBytesInWindow > kMaxInboundBytesPerWindow) ||
                (session.inboundPlayerInputsInWindow > kMaxPlayerInputsPerWindow) ||
                (session.inboundChunkRequestsInWindow > kMaxChunkRequestsPerWindow);
        }
    }
    if (exceededRateLimit) {
        std::cerr << "[recv] inbound rate limit exceeded conn=" << incoming << " (closing connection)\n";
        SteamNetworkingSockets()->CloseConnection(incoming, 0, "rate limit exceeded", false);
        return true;
    }
    return false;
}

void ServerNetwork::HandleConnectRequest(HSteamNetConnection incoming, const void* data, uint32_t size)
{
    auto sendResponse = [&](const ConnectResponse& response) {
        const std::vector<uint8_t> payload = response.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming,
            payload.data(),
            static_cast<uint32_t>(payload.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    };

    std::vector<uint8_t> connectBytes(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
    auto reqOpt = ConnectRequest::deserialize(connectBytes);
    if (!reqOpt.has_value()) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::InvalidPacket;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "invalid connect packet";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=invalid_packet\n";
        return;
    }

    ConnectRequest req = std::move(*reqOpt);
    if (req.protocolVersion != kVoxelOpsProtocolVersion) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::ProtocolMismatch;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "protocol mismatch: client=" + std::to_string(req.protocolVersion) +
            " server=" + std::to_string(kVoxelOpsProtocolVersion);
        sendResponse(response);
        std::cout
            << "[register rejected] conn=" << incoming
            << " reason=protocol_mismatch client=" << req.protocolVersion
            << " server=" << kVoxelOpsProtocolVersion << "\n";
        return;
    }

    const std::string identity = NormalizeIdentity(req.identity);
    if (!IsValidIdentity(identity)) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::InvalidIdentity;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "invalid identity";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=invalid_identity\n";
        return;
    }

    const std::string requestedUsername = NormalizeDisplayName(req.requestedUsername);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end() && it->second.playerId != 0 && !it->second.username.empty()) {
            ConnectResponse response;
            response.ok = 1;
            response.reason = ConnectRejectReason::None;
            response.serverProtocolVersion = kVoxelOpsProtocolVersion;
            response.assignedUsername = it->second.username;
            response.message = "already registered";
            sendResponse(response);
            std::cout
                << "[register] duplicate ConnectRequest ignored conn=" << incoming
                << " username=" << it->second.username << "\n";
            return;
        }

        for (const auto& [conn, session] : m_clients) {
            if (conn == incoming || session.playerId == 0 || session.identity.empty()) {
                continue;
            }
            if (session.identity == identity) {
                ConnectResponse response;
                response.ok = 0;
                response.reason = ConnectRejectReason::IdentityInUse;
                response.serverProtocolVersion = kVoxelOpsProtocolVersion;
                response.message = "identity already connected";
                sendResponse(response);
                std::cout
                    << "[register rejected] conn=" << incoming
                    << " identity=" << identity
                    << " reason=identity_in_use\n";
                return;
            }
        }

        if (!requestedUsername.empty()) {
            for (const auto& [conn, session] : m_clients) {
                if (conn == incoming || session.playerId == 0 || session.username.empty()) {
                    continue;
                }
                if (session.username == requestedUsername) {
                    ConnectResponse response;
                    response.ok = 0;
                    response.reason = ConnectRejectReason::UsernameTaken;
                    response.serverProtocolVersion = kVoxelOpsProtocolVersion;
                    response.message = "username already taken";
                    sendResponse(response);
                    std::cout
                        << "[register rejected] conn=" << incoming
                        << " requested=" << requestedUsername
                        << " reason=username_taken\n";
                    return;
                }
            }
        }
    }

    std::string username;
    if (!requestedUsername.empty()) {
        username = requestedUsername;
    }
    else {
        username = BuildDisplayNameForIdentityLocked(identity, requestedUsername, incoming);
    }
    if (username.empty()) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::ServerError;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "failed to allocate display name";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=name_allocation_failed\n";
        return;
    }

    auto connHandle = std::make_shared<ConnectionHandle>();
    connHandle->socketFd = static_cast<int>(incoming);
    const PlayerID playerId = m_playerManager.onPlayerConnect(connHandle, glm::vec3(0.0f, 60.0f, 0.0f));

    bool attached = false;
    bool sessionIsAdmin = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            it->second.identity = identity;
            it->second.username = username;
            it->second.playerId = playerId;
            it->second.isAdmin = (m_adminIdentities.find(identity) != m_adminIdentities.end());
            sessionIsAdmin = it->second.isAdmin;
            attached = true;
            m_matchScores[playerId] = MatchScore{};

            if (!m_matchStarted) {
                size_t activePlayers = 0;
                for (const auto& [_, session] : m_clients) {
                    if (session.playerId != 0) {
                        ++activePlayers;
                    }
                }
                if (activePlayers >= 2) {
                    m_matchStarted = true;
                    m_matchStartTime = std::chrono::steady_clock::now();
                    m_matchEnded = false;
                    m_matchWinner.clear();
                }
            }
        }
    }
    if (!attached) {
        m_playerManager.removePlayer(playerId);
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::ServerError;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "failed to attach session";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=attach_failed\n";
        return;
    }

    m_playerManager.setFlyModeAllowed(playerId, sessionIsAdmin);

    ConnectResponse response;
    response.ok = 1;
    response.reason = ConnectRejectReason::None;
    response.serverProtocolVersion = kVoxelOpsProtocolVersion;
    response.assignedUsername = username;
    response.message = "ok";
    sendResponse(response);

    std::string out;
    out.push_back(static_cast<char>(PacketType::ClientConnect));
    out += username;
    BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), incoming);

    std::cout
        << "[register] conn=" << incoming
        << " username=" << username
        << " identity=" << identity
        << " requested=" << requestedUsername << "\n";
}

void ServerNetwork::HandleMessagePacket(HSteamNetConnection incoming, const void* data, uint32_t size)
{
    std::string msg = ReadStringFromPacket(data, size, 1);
    std::string username;
    PlayerID playerId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            username = it->second.username;
            playerId = it->second.playerId;
        }
    }
    if (!username.empty()) {
        if (playerId != 0) {
            m_playerManager.touchHeartbeat(playerId);
        }

        if (msg == "RESPAWN") {
            if (playerId != 0) {
                (void)m_playerManager.requestRespawn(playerId);
            }
            return;
        }

        m_messageHistory.emplace_back(username, msg);
        std::string out;
        out.push_back(static_cast<char>(PacketType::Message));
        out += username;
        out.push_back(':');
        out += msg;
        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), incoming);
        std::cout << "[recv] " << username << ": " << msg << "\n";
    }
    else {
        std::cout << "[dropping] message from unregistered conn=" << incoming << "\n";
    }
}

void ServerNetwork::HandlePlayerInputPacket(
    HSteamNetConnection incoming,
    const void* data,
    uint32_t size,
    uint64_t& playerInputPacketsThisLoop
)
{
    ++playerInputPacketsThisLoop;
    PlayerInput input{};
    if (!ParsePlayerInputPacket(reinterpret_cast<const uint8_t*>(data), size, input)) {
        std::cout << "[recv] malformed PlayerInput (size=" << size << ")\n";
        return;
    }

    std::string username;
    PlayerID playerId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            username = it->second.username;
            playerId = it->second.playerId;
        }
    }
    if (!username.empty() && playerId != 0) {
        m_playerManager.enqueuePlayerInput(playerId, input);
        m_playerManager.setEquippedWeapon(playerId, input.weaponId);
    }
    else {
        std::cout << "[input] unregistered conn = " << incoming << " seq = " << input.sequenceNumber << "\n";
    }
}

void ServerNetwork::HandleChunkRequestPacket(
    HSteamNetConnection incoming,
    const void* data,
    uint32_t size,
    uint64_t& chunkRequestPacketsThisLoop
)
{
    ++chunkRequestPacketsThisLoop;
    ChunkRequest req{};
    if (!ParseChunkRequestPacket(reinterpret_cast<const uint8_t*>(data), size, req)) {
        std::cout << "[recv] malformed ChunkRequest (size=" << size << ")\n";
        return;
    }

    const glm::ivec3 centerChunk(req.chunkX, req.chunkY, req.chunkZ);
    const uint16_t clampedViewDistance = ClampViewDistance(req.viewDistance);
    bool registered = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end() && !it->second.username.empty() && it->second.playerId != 0) {
            const auto now = std::chrono::steady_clock::now();
            const bool hadInterest = it->second.hasChunkInterest;
            const bool centerChanged =
                !hadInterest ||
                it->second.interestCenterChunk.x != centerChunk.x ||
                it->second.interestCenterChunk.y != centerChunk.y ||
                it->second.interestCenterChunk.z != centerChunk.z;
            const bool viewChanged =
                !hadInterest ||
                it->second.viewDistance != clampedViewDistance;

            it->second.interestCenterChunk = centerChunk;
            it->second.viewDistance = clampedViewDistance;
            it->second.hasChunkInterest = true;

            if (
                centerChanged ||
                viewChanged ||
                now >= it->second.nextChunkInterestUpdateAt
            ) {
                it->second.chunkInterestDirty = true;
                it->second.nextChunkInterestUpdateAt = std::chrono::steady_clock::time_point::min();
            }

            registered = true;
        }
    }
    if (!registered) {
        return;
    }
}

void ServerNetwork::HandleChunkAckPacket(
    HSteamNetConnection incoming,
    const void* data,
    uint32_t size,
    uint64_t& chunkAckPacketsThisLoop
)
{
    ++chunkAckPacketsThisLoop;
    ChunkAck ack{};
    if (!ParseChunkAckPacket(reinterpret_cast<const uint8_t*>(data), size, ack)) {
        std::cerr << "[chunk/ack] malformed ChunkAck size=" << size << " conn=" << incoming << "\n";
        return;
    }

    if (ack.ackedType != static_cast<uint8_t>(PacketType::ChunkData)) {
        return;
    }

    const ChunkCoord coord{ ack.chunkX, ack.chunkY, ack.chunkZ };
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_clients.find(incoming);
    if (it == m_clients.end()) {
        return;
    }

    auto pendingIt = it->second.pendingChunkData.find(coord);
    auto expectedIt = it->second.pendingChunkDataPayloadHash.find(coord);
    const bool wasPending = (pendingIt != it->second.pendingChunkData.end());
    const bool wasStreamedAlready = it->second.streamedChunks.find(coord) != it->second.streamedChunks.end();
    const bool hadExpectedHash = (expectedIt != it->second.pendingChunkDataPayloadHash.end());
    const uint32_t expectedPayloadHash = hadExpectedHash ? expectedIt->second : 0;
    const bool hashMatches = !hadExpectedHash || (ack.sequence == expectedPayloadHash);

    if (wasPending && hashMatches) {
        it->second.pendingChunkData.erase(pendingIt);
        if (hadExpectedHash) {
            it->second.pendingChunkDataPayloadHash.erase(expectedIt);
        }
        it->second.streamedChunks.insert(coord);
    }
    else if (wasPending && !hashMatches) {
        // Keep pending so the chunk is retried; bypass retry cooldown for quick resend.
        pendingIt->second = std::chrono::steady_clock::time_point::min();
        std::cerr
            << "[chunk/ack] payload hash mismatch conn=" << incoming
            << " chunk=(" << coord.x << "," << coord.y << "," << coord.z << ")"
            << " expected=" << expectedPayloadHash
            << " got=" << ack.sequence
            << " version=" << ack.version << "\n";
    }
    else if (!wasPending && !wasStreamedAlready) {
        // Late ACK after server-side pruning/unload is expected during rapid movement.
        if (hadExpectedHash) {
            it->second.pendingChunkDataPayloadHash.erase(expectedIt);
        }
        static uint64_t unexpectedChunkAckCount = 0;
        ++unexpectedChunkAckCount;
        if (g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
            (unexpectedChunkAckCount <= 20 || (unexpectedChunkAckCount % 200) == 0)) {
            std::cerr
                << "[chunk/ack] ignored late/unexpected ChunkData ACK conn=" << incoming
                << " chunk=(" << coord.x << "," << coord.y << "," << coord.z << ")"
                << " seq=" << ack.sequence
                << " version=" << ack.version
                << " count=" << unexpectedChunkAckCount << "\n";
        }
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

    ShootRequest req{};
    if (!ParseShootRequestPacket(reinterpret_cast<const uint8_t*>(data), size, req)) {
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

    std::string username;
    PlayerID playerId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            username = it->second.username;
            playerId = it->second.playerId;
        }
    }

    const uint32_t currentServerTick = m_serverTick.load(std::memory_order_acquire);

    ShootResult res{};
    res.clientShotId = req.clientShotId;
    res.serverTick = currentServerTick;
    res.accepted = 0;
    res.didHit = 0;
    res.hitEntityId = -1;
    res.serverSeed = req.seed;
    res.newAmmoCount = 0;

    if (username.empty() || playerId == 0) {
        std::cout << "[recv] ShootRequest from unregistered conn = " << incoming << "\n";
        sendResult(res);
        return;
    }
    if (playerId != 0) {
        m_playerManager.touchHeartbeat(playerId);
        m_playerManager.setEquippedWeapon(playerId, req.weaponId);
    }

    bool rejectedReplay = false;
    bool rejectedCooldown = false;
    bool sessionMissing = false;
    const float minShotIntervalSeconds = MinSecondsPerShotForWeapon(req.weaponId);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it == m_clients.end()) {
            sessionMissing = true;
        }
        else {
            ClientSession& session = it->second;
            if (session.hasLastShootClientShotId &&
                !IsNewerU32(req.clientShotId, session.lastShootClientShotId)) {
                rejectedReplay = true;
            }
            else {
                session.lastShootClientShotId = req.clientShotId;
                session.hasLastShootClientShotId = true;

                const auto now = std::chrono::steady_clock::now();
                if (session.lastAcceptedShootTime != std::chrono::steady_clock::time_point::min()) {
                    const float elapsedSeconds = std::chrono::duration<float>(
                        now - session.lastAcceptedShootTime
                    ).count();
                    if (elapsedSeconds < minShotIntervalSeconds) {
                        rejectedCooldown = true;
                    }
                }
                if (!rejectedCooldown) {
                    session.lastAcceptedShootTime = now;
                }
            }
        }
    }
    if (sessionMissing) {
        sendResult(res);
        return;
    }
    if (rejectedReplay || rejectedCooldown) {
        if (kEnableShootValidationLogs) {
            std::cout
                << "[shoot/validate] result=rejected"
                << " reason=" << (rejectedReplay ? "replay_or_out_of_order" : "rate_limited")
                << " shotId=" << req.clientShotId
                << "\n";
        }
        sendResult(res);
        return;
    }

    const LagCompFrame* lagCompFrame = nullptr;
    uint32_t lagCompTargetTick = currentServerTick;
    if (req.clientTick <= currentServerTick &&
        (currentServerTick - req.clientTick) <= kShootLagCompensationMaxTicks) {
        lagCompTargetTick = req.clientTick;
    }
    if (!m_lagCompFrames.empty()) {
        for (auto it = m_lagCompFrames.rbegin(); it != m_lagCompFrames.rend(); ++it) {
            if (!IsNewerU32(it->serverTick, lagCompTargetTick)) {
                lagCompFrame = &(*it);
                break;
            }
        }
    }

    auto shooterOpt = m_playerManager.getPlayerCopy(playerId);
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

    glm::vec3 shooterBasePos = shooter.position;
    if (lagCompFrame != nullptr) {
        auto shooterLagIt = lagCompFrame->players.find(playerId);
        if (shooterLagIt != lagCompFrame->players.end()) {
            shooterBasePos = shooterLagIt->second.position;
        }
    }

    const glm::vec3 rayDir = glm::normalize(requestDir);
    const glm::vec3 shooterEyePos = shooterBasePos + glm::vec3(0.0f, movementSettings().eyeHeight, 0.0f);
    const glm::vec3 requestPos(req.posX, req.posY, req.posZ);
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
    const glm::vec3 rayOrigin = allowRequestedOrigin ? requestPos : shooterEyePos;
    const float maxDistance = kShootMaxDistance;
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] shooter=" << playerId
            << " lagCompTick=" << (lagCompFrame ? static_cast<int64_t>(lagCompFrame->serverTick) : -1)
            << " origin=(" << rayOrigin.x << "," << rayOrigin.y << "," << rayOrigin.z << ")"
            << " requestedOriginAccepted=" << (allowRequestedOrigin ? "yes" : "no")
            << " maxDistance=" << maxDistance
            << "\n";
    }

    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    bool playerHit = false;
    PlayerID hitPlayerId = 0;
    HitRegion hitRegion = HitRegion::Unknown;
    glm::vec3 hitPoint = rayOrigin + rayDir * maxDistance;
    float bestPlayerDistance = maxDistance + 1.0f;

    for (const ServerPlayer& target : players) {
        if (target.id == playerId || !target.isAlive) {
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

        const glm::mat4 targetModel = BuildPlayerModelMatrix(targetPosition, targetYaw);
        float meshReferenceHeight = 0.0f;
        const std::vector<MeshHitTriangle>& meshTriangles = GetSharedPlayerMeshHitTriangles(meshReferenceHeight);
        HitResult hit;
        if (!meshTriangles.empty()) {
            const float uniformScale = (meshReferenceHeight > 1e-4f) ? (targetHeight / meshReferenceHeight) : 1.0f;
            hit = RaycastMeshTriangles(rayOrigin, rayDir, meshTriangles, targetModel, uniformScale, maxDistance);
        }
        else {
            float referenceHeight = 0.0f;
            float referenceRadius = 0.0f;
            const std::vector<Hitbox>& baseHitboxes =
                GetSharedPlayerHitboxes(referenceHeight, referenceRadius);
            const float sx = (referenceRadius > 1e-4f) ? (targetRadius / referenceRadius) : 1.0f;
            const float sy = (referenceHeight > 1e-4f) ? (targetHeight / referenceHeight) : 1.0f;
            const float sz = sx;

            std::vector<Hitbox> scaledHitboxes;
            scaledHitboxes.reserve(baseHitboxes.size());
            for (const Hitbox& base : baseHitboxes) {
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
                targetModel,
                maxDistance
            );
        }
        const HitRegion resolvedRegion = hit.hit
            ? ResolveHitRegionByPoint(hit.hitPointWorld, targetModel, targetHeight, hit.region)
            : HitRegion::Unknown;
        if (kEnableShootValidationLogs && hit.hit) {
            std::cout
                << "[shoot/validate] candidate player=" << target.id
                << " dist=" << hit.distance
                << " region=" << HitRegionName(resolvedRegion)
                << " hit=(" << hit.hitPointWorld.x << "," << hit.hitPointWorld.y << "," << hit.hitPointWorld.z << ")"
                << "\n";
        }
        if (hit.hit && hit.distance < bestPlayerDistance) {
            playerHit = true;
            hitPlayerId = target.id;
            hitRegion = resolvedRegion;
            hitPoint = hit.hitPointWorld;
            bestPlayerDistance = hit.distance;
        }
    }

    float blockDistance = maxDistance + 1.0f;
    glm::vec3 blockHitPoint(0.0f);
    const bool blockHit = FindFirstSolidBlockHit(
        m_chunkManager,
        rayOrigin,
        rayDir,
        maxDistance,
        blockDistance,
        blockHitPoint
    );
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] nearest playerHit=" << (playerHit ? "yes" : "no")
            << " playerDist=" << (playerHit ? bestPlayerDistance : -1.0f)
            << " blockHit=" << (blockHit ? "yes" : "no")
            << " blockDist=" << (blockHit ? blockDistance : -1.0f)
            << "\n";
    }

    res.accepted = 1;
    if (!playerHit || (blockHit && (blockDistance + kShootBlockOcclusionEpsilon) <= bestPlayerDistance)) {
        res.didHit = 0;
        const glm::vec3 endpoint = blockHit ? blockHitPoint : (rayOrigin + rayDir * maxDistance);
        res.hitX = endpoint.x;
        res.hitY = endpoint.y;
        res.hitZ = endpoint.z;
        if (kEnableShootValidationLogs) {
            const char* reason = (!playerHit) ? "no_player_intersection" : "occluded_by_block";
            std::cout
                << "[shoot/validate] result=miss reason=" << reason
                << " blockDist=" << (blockHit ? blockDistance : -1.0f)
                << " playerDist=" << (playerHit ? bestPlayerDistance : -1.0f)
                << " epsilon=" << kShootBlockOcclusionEpsilon
                << " endpoint=(" << endpoint.x << "," << endpoint.y << "," << endpoint.z << ")"
                << "\n";
        }
        sendResult(res);
        return;
    }

    float damage = 0;
    switch (hitRegion) {
    case HitRegion::Head:
        damage = HeadshotDamageForWeapon(req.weaponId);
        break;
    case HitRegion::Body:
        damage = TorsoshotDamageForWeapon(req.weaponId);
        break;
    case HitRegion::Legs:
        damage = LegshotDamageForWeapon(req.weaponId);
        break;
    case HitRegion::Unknown:
    default:
        // Unknown region should still apply deterministic damage.
        // Use torso as neutral fallback.
        damage = TorsoshotDamageForWeapon(req.weaponId);
        break;
    }

    float healthAfter = 0.0f;
    bool killed = false;
    if (!m_playerManager.applyDamage(hitPlayerId, damage, healthAfter, killed)) {
        res.didHit = 0;
        res.hitX = hitPoint.x;
        res.hitY = hitPoint.y;
        res.hitZ = hitPoint.z;
        if (kEnableShootValidationLogs) {
            std::cout
                << "[shoot/validate] result=miss reason=apply_damage_failed"
                << " target=" << hitPlayerId
                << "\n";
        }
        sendResult(res);
        return;
    }

    res.didHit = 1;
    res.hitEntityId = (hitPlayerId <= static_cast<PlayerID>(std::numeric_limits<int32_t>::max()))
        ? static_cast<int32_t>(hitPlayerId)
        : -1;
    res.hitX = hitPoint.x;
    res.hitY = hitPoint.y;
    res.hitZ = hitPoint.z;
    res.damageApplied = damage;
    if (kEnableShootValidationLogs) {
        std::cout
            << "[shoot/validate] result=hit"
            << " target=" << hitPlayerId
            << " region=" << HitRegionName(hitRegion)
            << " damage=" << damage
            << " healthAfter=" << healthAfter
            << " killed=" << (killed ? "yes" : "no")
            << " point=(" << hitPoint.x << "," << hitPoint.y << "," << hitPoint.z << ")"
            << "\n";
    }

    if (killed) {
        std::string victimUsername;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_matchEnded) {
                auto killerIt = m_matchScores.find(playerId);
                if (killerIt != m_matchScores.end()) {
                    ++killerIt->second.kills;
                }
                auto victimIt = m_matchScores.find(hitPlayerId);
                if (victimIt != m_matchScores.end()) {
                    ++victimIt->second.deaths;
                }
            }
            for (const auto& [_, session] : m_clients) {
                if (session.playerId == hitPlayerId) {
                    victimUsername = session.username;
                    break;
                }
            }
        }
        if (victimUsername.empty()) {
            victimUsername = std::string("Player") + std::to_string(hitPlayerId);
        }

        std::string killPayload = "KILLFEED|";
        killPayload += username;
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

void ServerNetwork::DispatchInboundPacket(
    HSteamNetConnection incoming,
    PacketType packetType,
    const void* data,
    uint32_t size,
    uint64_t& playerInputPacketsThisLoop,
    uint64_t& chunkRequestPacketsThisLoop,
    uint64_t& chunkAckPacketsThisLoop
)
{
    switch (packetType) {
    case PacketType::ConnectRequest:
        HandleConnectRequest(incoming, data, size);
        return;
    case PacketType::Message:
        HandleMessagePacket(incoming, data, size);
        return;
    case PacketType::PlayerInput:
        HandlePlayerInputPacket(incoming, data, size, playerInputPacketsThisLoop);
        return;
    case PacketType::PlayerPosition:
        // Legacy packet still accepted on the wire but ignored by authoritative movement.
        return;
    case PacketType::ChunkRequest:
        HandleChunkRequestPacket(incoming, data, size, chunkRequestPacketsThisLoop);
        return;
    case PacketType::ChunkAck:
        HandleChunkAckPacket(incoming, data, size, chunkAckPacketsThisLoop);
        return;
    case PacketType::ShootRequest:
        HandleShootRequestPacket(incoming, data, size);
        return;
    default:
        return;
    }
}

void ServerNetwork::MainLoop()
{
    auto lastFrameTime = std::chrono::steady_clock::now();
    auto lastSnapshotTime = lastFrameTime;
    constexpr double kServerTickSeconds = 1.0 / static_cast<double>(kServerTickRateHz);
    constexpr size_t kMaxSimCatchupTicksPerLoop = 4;
    constexpr size_t kChunkInterestUpdatesPerLoop = 4;
    constexpr size_t kMaxInboundMessagesPerLoop = 256;
    constexpr int64_t kInboundMessageBudgetUs = 3000;
    const auto kChunkInterestUpdateInterval = std::chrono::milliseconds(100);
    constexpr int kCollisionPrewarmRadiusXZ = 1;
    constexpr int kCollisionPrewarmRadiusY = 1;
    constexpr size_t kMaxCollisionPrewarmGenerationsPerLoop = 8;
    constexpr int64_t kCollisionPrewarmBudgetUs = 1500;
    const auto kCollisionPrewarmInterval = std::chrono::milliseconds(50);
    constexpr size_t kChunkSendGlobalBudgetPerFlush = 8;
    constexpr size_t kChunkSendPerClientBudgetPerFlush = 4;
    const auto kChunkSendFlushInterval = std::chrono::milliseconds(16);
    const auto kScoreboardBroadcastInterval = std::chrono::seconds(1);
    const auto snapshotInterval = std::chrono::duration<double>(kServerTickSeconds);
    double simAccumulator = 0.0;
    uint32_t serverTick = 0;
    auto nextChunkSendFlushAt = std::chrono::steady_clock::now();
    auto nextCollisionPrewarmAt = std::chrono::steady_clock::now();
    auto perfWindowStart = std::chrono::steady_clock::now();
    uint64_t perfLoops = 0;
    uint64_t perfMessages = 0;
    uint64_t perfPlayerInputs = 0;
    uint64_t perfChunkRequests = 0;
    uint64_t perfChunkAcks = 0;
    uint64_t perfSimTicks = 0;
    uint64_t perfCollisionPrewarmGenerated = 0;
    uint64_t perfChunkInterestTasks = 0;
    uint64_t perfChunksSent = 0;
    uint64_t perfScoreboardBroadcasts = 0;
    double perfLoopUsTotal = 0.0;
    double perfLoopUsMax = 0.0;
    double perfMessageDrainUsTotal = 0.0;
    double perfSimUsTotal = 0.0;
    double perfCollisionPrewarmUsTotal = 0.0;
    double perfSnapshotUsTotal = 0.0;
    double perfChunkInterestUsTotal = 0.0;
    double perfChunkSendUsTotal = 0.0;
    auto nextScoreboardBroadcastAt = std::chrono::steady_clock::now();

    while (!m_quit) {
        const auto loopStart = std::chrono::steady_clock::now();
        uint64_t msgPacketsThisLoop = 0;
        uint64_t playerInputPacketsThisLoop = 0;
        uint64_t chunkRequestPacketsThisLoop = 0;
        uint64_t chunkAckPacketsThisLoop = 0;

        const auto frameNow = std::chrono::steady_clock::now();
        double deltaSeconds = std::chrono::duration<double>(frameNow - lastFrameTime).count();
        if (deltaSeconds > 0.25) {
            deltaSeconds = 0.25;
        }
        lastFrameTime = frameNow;
        simAccumulator += deltaSeconds;

        SteamNetworkingSockets()->RunCallbacks();

        // Receive messages on poll group (any connection assigned to it).
        // Drain with a bounded budget so message bursts do not starve simulation ticks.
        const auto messageDrainStart = std::chrono::steady_clock::now();
        SteamNetworkingMessage_t* pMsg = nullptr;
        size_t inboundMessagesProcessed = 0;
        const auto reachedMessageDrainBudget = [&]() {
            ++inboundMessagesProcessed;
            const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - messageDrainStart
            ).count();
            return elapsedUs >= kInboundMessageBudgetUs;
        };
        while (
            inboundMessagesProcessed < kMaxInboundMessagesPerLoop &&
            SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, &pMsg, 1) > 0 &&
            pMsg
        ) {
            HSteamNetConnection incoming = pMsg->m_conn;
            const void* data = pMsg->m_pData;
            const uint32_t cb = pMsg->m_cbSize;

            if (cb < 1 || cb > kMaxInboundPacketBytes) {
                std::cerr
                    << "[recv] invalid packet size=" << cb
                    << " conn=" << incoming
                    << " (closing connection)\n";
                SteamNetworkingSockets()->CloseConnection(incoming, 0, "invalid packet size", false);
                pMsg->Release();
                if (reachedMessageDrainBudget()) {
                    break;
                }
                continue;
            }

            const PacketType packetType = static_cast<PacketType>(reinterpret_cast<const uint8_t*>(data)[0]);
            if (!IsInboundPacketSizeValid(packetType, cb)) {
                std::cerr
                    << "[recv] packet size/type mismatch type=" << static_cast<int>(packetType)
                    << " size=" << cb
                    << " conn=" << incoming << "\n";
                pMsg->Release();
                if (reachedMessageDrainBudget()) {
                    break;
                }
                continue;
            }

            if (IsInboundRateLimitExceeded(incoming, packetType, cb)) {
                pMsg->Release();
                if (reachedMessageDrainBudget()) {
                    break;
                }
                continue;
            }

            ++msgPacketsThisLoop;
            DispatchInboundPacket(
                incoming,
                packetType,
                data,
                cb,
                playerInputPacketsThisLoop,
                chunkRequestPacketsThisLoop,
                chunkAckPacketsThisLoop
            );
            pMsg->Release();
            if (reachedMessageDrainBudget()) {
                break;
            }
        }

        // Optional: extra safeguard - check connection states for any connections left (callback already handles most)
        std::vector<std::pair<HSteamNetConnection, ClientSession>> staleConnections;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_clients.begin(); it != m_clients.end();) {
                HSteamNetConnection conn = it->first;
                SteamNetConnectionInfo_t info;
                if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info) &&
                    (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)) {
                    staleConnections.emplace_back(conn, it->second);
                    it = m_clients.erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (const auto& [conn, session] : staleConnections) {
            std::cout << "[cleanup] remove conn=" << conn << " user=" << session.username << "\n";
            ClearChunkPipelineForConnection(conn);
            if (session.playerId != 0) {
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_matchScores.erase(session.playerId);
                }
                m_playerManager.removePlayer(session.playerId);
            }
            if (!session.username.empty()) {
                std::string out;
                out.push_back(static_cast<char>(PacketType::ClientDisconnect));
                out += session.username;
                BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), conn);
            }
            SteamNetworkingSockets()->CloseConnection(conn, 0, "server cleanup", false);
        }
        const double messageDrainUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - messageDrainStart
            ).count()
        );

        const auto simStart = std::chrono::steady_clock::now();
        uint64_t simTicksThisLoop = 0;
        while (
            simAccumulator >= kServerTickSeconds &&
            simTicksThisLoop < kMaxSimCatchupTicksPerLoop
        ) {
            m_playerManager.update(kServerTickSeconds, m_chunkManager);
            simAccumulator -= kServerTickSeconds;
            ++serverTick;
            m_serverTick.store(serverTick, std::memory_order_release);
            RecordLagCompFrame(serverTick);
            ++simTicksThisLoop;
        }
        const double simUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - simStart
            ).count()
        );
        const bool simBacklog = (simAccumulator >= kServerTickSeconds);
        size_t collisionPrewarmGeneratedThisLoop = 0;
        double collisionPrewarmUs = 0.0;

        const auto snapshotNow = std::chrono::steady_clock::now();
        const auto snapshotStart = std::chrono::steady_clock::now();
        bool snapshotRan = false;
        if (snapshotNow - lastSnapshotTime >= snapshotInterval) {
            snapshotRan = true;
            lastSnapshotTime = snapshotNow;

            std::vector<std::pair<HSteamNetConnection, PlayerID>> recipients;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                recipients.reserve(m_clients.size());
                for (const auto& [conn, session] : m_clients) {
                    if (session.playerId != 0) {
                        recipients.emplace_back(conn, session.playerId);
                    }
                }
            }

            std::vector<HSteamNetConnection> staleRecipients;
            std::vector<PlayerID> recipientIds;
            recipientIds.reserve(recipients.size());
            for (const auto& [_, playerId] : recipients) {
                recipientIds.push_back(playerId);
            }
            std::vector<std::vector<uint8_t>> snapshots =
                m_playerManager.buildSnapshotsForRecipients(recipientIds, serverTick);

            const size_t snapshotCount = std::min(recipients.size(), snapshots.size());
            for (size_t i = 0; i < snapshotCount; ++i) {
                const HSteamNetConnection conn = recipients[i].first;
                std::vector<uint8_t>& snapshot = snapshots[i];
                if (snapshot.empty()) {
                    staleRecipients.push_back(conn);
                    continue;
                }
                SteamNetworkingSockets()->SendMessageToConnection(
                    conn,
                    snapshot.data(),
                    static_cast<uint32_t>(snapshot.size()),
                    k_nSteamNetworkingSend_UnreliableNoDelay,
                    nullptr
                );
            }

            if (!staleRecipients.empty()) {
                std::vector<std::pair<HSteamNetConnection, ClientSession>> removedSessions;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    for (HSteamNetConnection conn : staleRecipients) {
                        auto it = m_clients.find(conn);
                    if (it != m_clients.end()) {
                        removedSessions.emplace_back(it->first, it->second);
                            m_clients.erase(it);
                        }
                    }
                }

                for (const auto& [conn, session] : removedSessions) {
                    ClearChunkPipelineForConnection(conn);
                    if (session.playerId != 0) {
                        std::lock_guard<std::mutex> lk(m_mutex);
                        m_matchScores.erase(session.playerId);
                    }
                    if (!session.username.empty()) {
                        std::string out;
                        out.push_back(static_cast<char>(PacketType::ClientDisconnect));
                        out += session.username;
                        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), conn);
                    }
                    SteamNetworkingSockets()->CloseConnection(conn, 0, "server player timeout", false);
                }
            }
        }
        const double snapshotUs = snapshotRan
            ? static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - snapshotStart
                ).count()
              )
            : 0.0;

        bool scoreboardBroadcastedThisLoop = false;
        const auto scoreboardNow = std::chrono::steady_clock::now();
        if (scoreboardNow >= nextScoreboardBroadcastAt) {
            std::string scoreboardPayload;
            std::string endAnnouncementPayload;
            {
                std::lock_guard<std::mutex> lk(m_mutex);

                int remainingSec = static_cast<int>(m_matchDuration.count());
                if (m_matchStarted) {
                    const int64_t elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
                        scoreboardNow - m_matchStartTime
                    ).count();
                    const int64_t remainingSecRaw =
                        static_cast<int64_t>(m_matchDuration.count()) - elapsedSec;
                    remainingSec = static_cast<int>(std::max<int64_t>(0, remainingSecRaw));
                }

                if (m_matchStarted && !m_matchEnded && remainingSec <= 0) {
                    m_matchEnded = true;

                    struct WinnerCandidate {
                        std::string username;
                        uint32_t kills = 0;
                    };
                    std::vector<WinnerCandidate> candidates;
                    candidates.reserve(m_clients.size());
                    for (const auto& [_, session] : m_clients) {
                        if (session.playerId == 0 || session.username.empty()) {
                            continue;
                        }
                        uint32_t kills = 0;
                        auto scoreIt = m_matchScores.find(session.playerId);
                        if (scoreIt != m_matchScores.end()) {
                            kills = scoreIt->second.kills;
                        }
                        candidates.push_back(WinnerCandidate{ session.username, kills });
                    }
                    std::sort(
                        candidates.begin(),
                        candidates.end(),
                        [](const WinnerCandidate& a, const WinnerCandidate& b) {
                            if (a.kills != b.kills) {
                                return a.kills > b.kills;
                            }
                            return a.username < b.username;
                        }
                    );

                    if (candidates.empty()) {
                        m_matchWinner = "No winner";
                    }
                    else if (candidates.size() >= 2 && candidates[0].kills == candidates[1].kills) {
                        m_matchWinner = "Tie";
                    }
                    else {
                        m_matchWinner = candidates[0].username;
                    }

                    endAnnouncementPayload = "MATCH_END|";
                    endAnnouncementPayload += m_matchWinner;
                }

                struct ScoreboardRow {
                    std::string username;
                    uint32_t kills = 0;
                    uint32_t deaths = 0;
                    int pingMs = -1;
                };

                std::vector<ScoreboardRow> rows;
                rows.reserve(m_clients.size());
                for (const auto& [conn, session] : m_clients) {
                    if (session.playerId == 0 || session.username.empty()) {
                        continue;
                    }

                    ScoreboardRow row;
                    row.username = session.username;
                    auto scoreIt = m_matchScores.find(session.playerId);
                    if (scoreIt != m_matchScores.end()) {
                        row.kills = scoreIt->second.kills;
                        row.deaths = scoreIt->second.deaths;
                    }

                    SteamNetConnectionRealTimeStatus_t status{};
                    const EResult pingResult = SteamNetworkingSockets()->GetConnectionRealTimeStatus(
                        conn,
                        &status,
                        0,
                        nullptr
                    );
                    if (pingResult == k_EResultOK) {
                        row.pingMs = status.m_nPing;
                    }

                    rows.push_back(std::move(row));
                }

                std::sort(
                    rows.begin(),
                    rows.end(),
                    [](const ScoreboardRow& a, const ScoreboardRow& b) {
                        if (a.kills != b.kills) {
                            return a.kills > b.kills;
                        }
                        if (a.deaths != b.deaths) {
                            return a.deaths < b.deaths;
                        }
                        return a.username < b.username;
                    }
                );

                scoreboardPayload.reserve(64 + rows.size() * 32);
                scoreboardPayload += "SCOREBOARD|";
                scoreboardPayload += std::to_string(remainingSec);
                scoreboardPayload += "|";
                scoreboardPayload += (m_matchEnded ? "1" : "0");
                scoreboardPayload += "|";
                scoreboardPayload += (m_matchStarted ? "1" : "0");
                scoreboardPayload += "|";
                scoreboardPayload += m_matchWinner.empty() ? "-" : m_matchWinner;
                scoreboardPayload += "|";
                scoreboardPayload += std::to_string(rows.size());
                for (const ScoreboardRow& row : rows) {
                    scoreboardPayload += "|";
                    scoreboardPayload += row.username;
                    scoreboardPayload += ",";
                    scoreboardPayload += std::to_string(row.kills);
                    scoreboardPayload += ",";
                    scoreboardPayload += std::to_string(row.deaths);
                    scoreboardPayload += ",";
                    scoreboardPayload += std::to_string(row.pingMs);
                }
            }

            if (!endAnnouncementPayload.empty()) {
                std::string out;
                out.reserve(1 + endAnnouncementPayload.size());
                out.push_back(static_cast<char>(PacketType::Message));
                out += endAnnouncementPayload;
                BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
            }

            if (!scoreboardPayload.empty()) {
                std::string out;
                out.reserve(1 + scoreboardPayload.size());
                out.push_back(static_cast<char>(PacketType::Message));
                out += scoreboardPayload;
                BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), k_HSteamNetConnection_Invalid);
            }

            nextScoreboardBroadcastAt = scoreboardNow + kScoreboardBroadcastInterval;
            scoreboardBroadcastedThisLoop = true;
        }

        struct ChunkInterestTask {
            HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
            glm::ivec3 centerChunk{ 0 };
            uint16_t viewDistance = 0;
        };
        std::vector<ChunkInterestTask> chunkInterestTasks;
        double chunkInterestUs = 0.0;
        if (!simBacklog) {
            const auto chunkInterestStart = std::chrono::steady_clock::now();
            const auto chunkInterestNow = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                chunkInterestTasks.reserve(
                    std::min<size_t>(kChunkInterestUpdatesPerLoop, m_clients.size())
                );
                for (auto& [conn, session] : m_clients) {
                    if (!session.hasChunkInterest) {
                        continue;
                    }
                    if (!session.chunkInterestDirty && chunkInterestNow < session.nextChunkInterestUpdateAt) {
                        continue;
                    }

                    chunkInterestTasks.push_back(ChunkInterestTask{
                        conn,
                        session.interestCenterChunk,
                        session.viewDistance
                    });
                    session.chunkInterestDirty = false;
                    session.nextChunkInterestUpdateAt = chunkInterestNow + kChunkInterestUpdateInterval;

                    if (chunkInterestTasks.size() >= kChunkInterestUpdatesPerLoop) {
                        break;
                    }
                }
            }
            for (const ChunkInterestTask& task : chunkInterestTasks) {
                UpdateChunkStreamingForClient(task.conn, task.centerChunk, task.viewDistance);
            }
            chunkInterestUs = static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - chunkInterestStart
                ).count()
            );
        }

        size_t chunksSentThisLoop = 0;
        double chunkSendUs = 0.0;
        if (!simBacklog) {
            const auto chunkSendNow = std::chrono::steady_clock::now();
            if (chunkSendNow >= nextChunkSendFlushAt) {
                const auto chunkSendStart = std::chrono::steady_clock::now();
                chunksSentThisLoop = FlushChunkSendQueues(
                    kChunkSendGlobalBudgetPerFlush,
                    kChunkSendPerClientBudgetPerFlush
                );
                chunkSendUs = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - chunkSendStart
                    ).count()
                );
                nextChunkSendFlushAt = chunkSendNow + kChunkSendFlushInterval;
            }
        }

        if (!simBacklog) {
            const auto prewarmNow = std::chrono::steady_clock::now();
            if (prewarmNow >= nextCollisionPrewarmAt) {
                const auto collisionPrewarmStart = prewarmNow;
                std::vector<PlayerID> activePlayerIds;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    activePlayerIds.reserve(m_clients.size());
                    for (const auto& [_, session] : m_clients) {
                        if (session.playerId != 0) {
                            activePlayerIds.push_back(session.playerId);
                        }
                    }
                }

                for (PlayerID playerId : activePlayerIds) {
                    if (collisionPrewarmGeneratedThisLoop >= kMaxCollisionPrewarmGenerationsPerLoop) {
                        break;
                    }
                    const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - collisionPrewarmStart
                    ).count();
                    if (elapsedUs >= kCollisionPrewarmBudgetUs) {
                        break;
                    }

                    const auto playerOpt = m_playerManager.getPlayerCopy(playerId);
                    if (!playerOpt.has_value()) {
                        continue;
                    }

                    const ServerPlayer& player = *playerOpt;
                    const glm::ivec3 playerWorldPos(
                        static_cast<int>(std::floor(player.position.x)),
                        static_cast<int>(std::floor(player.position.y)),
                        static_cast<int>(std::floor(player.position.z))
                    );
                    const glm::ivec3 centerChunk = m_chunkManager.worldToChunkPos(playerWorldPos);

                    bool hitLoopBudget = false;
                    for (int dx = -kCollisionPrewarmRadiusXZ; dx <= kCollisionPrewarmRadiusXZ && !hitLoopBudget; ++dx) {
                        for (int dz = -kCollisionPrewarmRadiusXZ; dz <= kCollisionPrewarmRadiusXZ && !hitLoopBudget; ++dz) {
                            for (int dy = -kCollisionPrewarmRadiusY; dy <= kCollisionPrewarmRadiusY; ++dy) {
                                const int64_t innerElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - collisionPrewarmStart
                                ).count();
                                if (innerElapsedUs >= kCollisionPrewarmBudgetUs) {
                                    hitLoopBudget = true;
                                    break;
                                }

                                const glm::ivec3 chunkPos(centerChunk.x + dx, centerChunk.y + dy, centerChunk.z + dz);
                                if (!m_chunkManager.inBounds(chunkPos)) {
                                    continue;
                                }
                                if (m_chunkManager.hasChunkLoaded(chunkPos)) {
                                    continue;
                                }
                                m_chunkManager.generateTerrainChunkAt(chunkPos);
                                ++collisionPrewarmGeneratedThisLoop;
                                if (collisionPrewarmGeneratedThisLoop >= kMaxCollisionPrewarmGenerationsPerLoop) {
                                    hitLoopBudget = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                collisionPrewarmUs = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - collisionPrewarmStart
                    ).count()
                );
                nextCollisionPrewarmAt = std::chrono::steady_clock::now() + kCollisionPrewarmInterval;
            }
        }
        else {
            // Sim tick is behind: defer background world work and retry once backlog clears.
            nextCollisionPrewarmAt = std::chrono::steady_clock::now();
        }

        const double loopUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - loopStart
            ).count()
        );
        if (loopUs > perfLoopUsMax) {
            perfLoopUsMax = loopUs;
        }
        ++perfLoops;
        perfMessages += msgPacketsThisLoop;
        perfPlayerInputs += playerInputPacketsThisLoop;
        perfChunkRequests += chunkRequestPacketsThisLoop;
        perfChunkAcks += chunkAckPacketsThisLoop;
        perfSimTicks += simTicksThisLoop;
        perfCollisionPrewarmGenerated += collisionPrewarmGeneratedThisLoop;
        perfChunkInterestTasks += chunkInterestTasks.size();
        perfChunksSent += chunksSentThisLoop;
        if (scoreboardBroadcastedThisLoop) {
            ++perfScoreboardBroadcasts;
        }
        perfLoopUsTotal += loopUs;
        perfMessageDrainUsTotal += messageDrainUs;
        perfSimUsTotal += simUs;
        perfCollisionPrewarmUsTotal += collisionPrewarmUs;
        perfSnapshotUsTotal += snapshotUs;
        perfChunkInterestUsTotal += chunkInterestUs;
        perfChunkSendUsTotal += chunkSendUs;

        if (g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (loopUs >= kSlowServerLoopWarnUs || simUs >= kSlowServerSimWarnUs)) {
            std::cerr
                << "[perf/server] slow loopUs=" << loopUs
                << " simUs=" << simUs
                << " msgDrainUs=" << messageDrainUs
                << " prewarmUs=" << collisionPrewarmUs
                << " snapshotUs=" << snapshotUs
                << " chunkInterestUs=" << chunkInterestUs
                << " chunkSendUs=" << chunkSendUs
                << " simTicks=" << simTicksThisLoop
                << " msgs=" << msgPacketsThisLoop
                << " inputs=" << playerInputPacketsThisLoop
                << " chunkReq=" << chunkRequestPacketsThisLoop
                << " chunkAck=" << chunkAckPacketsThisLoop
                << " prewarmGenerated=" << collisionPrewarmGeneratedThisLoop
                << " chunkInterestTasks=" << chunkInterestTasks.size()
                << " chunksSent=" << chunksSentThisLoop
                << "\n";
        }

        const auto perfNow = std::chrono::steady_clock::now();
        if (g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
            (perfNow - perfWindowStart) >= kServerPerfLogInterval) {
            const double loops = (perfLoops > 0) ? static_cast<double>(perfLoops) : 1.0;
            std::cerr
                << "[perf/server] 1s loops=" << perfLoops
                << " avgLoopMs=" << (perfLoopUsTotal / loops) / 1000.0
                << " maxLoopMs=" << perfLoopUsMax / 1000.0
                << " avgMsgDrainMs=" << (perfMessageDrainUsTotal / loops) / 1000.0
                << " avgSimMs=" << (perfSimUsTotal / loops) / 1000.0
                << " avgPrewarmMs=" << (perfCollisionPrewarmUsTotal / loops) / 1000.0
                << " avgSnapshotMs=" << (perfSnapshotUsTotal / loops) / 1000.0
                << " avgChunkInterestMs=" << (perfChunkInterestUsTotal / loops) / 1000.0
                << " avgChunkSendMs=" << (perfChunkSendUsTotal / loops) / 1000.0
                << " simTicks=" << perfSimTicks
                << " msgs=" << perfMessages
                << " inputs=" << perfPlayerInputs
                << " chunkReq=" << perfChunkRequests
                << " chunkAck=" << perfChunkAcks
                << " prewarmGenerated=" << perfCollisionPrewarmGenerated
                << " chunkInterestTasks=" << perfChunkInterestTasks
                << " chunksSent=" << perfChunksSent
                << " scoreboardBroadcasts=" << perfScoreboardBroadcasts
                << "\n";

            perfWindowStart = perfNow;
            perfLoops = 0;
            perfMessages = 0;
            perfPlayerInputs = 0;
            perfChunkRequests = 0;
            perfChunkAcks = 0;
            perfSimTicks = 0;
            perfCollisionPrewarmGenerated = 0;
            perfChunkInterestTasks = 0;
            perfChunksSent = 0;
            perfScoreboardBroadcasts = 0;
            perfLoopUsTotal = 0.0;
            perfLoopUsMax = 0.0;
            perfMessageDrainUsTotal = 0.0;
            perfSimUsTotal = 0.0;
            perfCollisionPrewarmUsTotal = 0.0;
            perfSnapshotUsTotal = 0.0;
            perfChunkInterestUsTotal = 0.0;
            perfChunkSendUsTotal = 0.0;
        }

        if (simBacklog) {
            std::this_thread::yield();
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

    }
}

// Broadcast raw payload to everyone except `except`
void ServerNetwork::SetDebugLoggingEnabled(bool enabled)
{
    g_enableChunkDiagnostics.store(enabled, std::memory_order_release);
    g_enableServerPerfDiagnostics.store(enabled, std::memory_order_release);
    m_playerManager.SetDebugLoggingEnabled(enabled);
    std::cout << "[debug] diagnostics " << (enabled ? "enabled" : "disabled") << "\n";
}

bool ServerNetwork::IsDebugLoggingEnabled()
{
    if (g_enableChunkDiagnostics.load(std::memory_order_acquire) ||
        g_enableServerPerfDiagnostics.load(std::memory_order_acquire)) {
        return true;
    }
    return m_playerManager.IsDebugLoggingEnabled();
}


