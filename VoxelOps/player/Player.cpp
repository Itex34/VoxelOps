#include "Player.hpp"

#include "../graphics/ChunkManager.hpp"
#include "../graphics/Shader.hpp"
#include "../data/GameData.hpp"
#include "../../Shared/network/Packets.hpp"
#include "../../Shared/player/HitboxCache.hpp"
#include "../../Shared/player/MeshHitCache.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/player/MovementSimulation.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <GLFW/glfw3.h> // only in .cpp
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
    inline const Shared::PlayerData::MovementSettings& movementSettings() {
        return Shared::PlayerData::GetMovementSettings();
    }
    // Match authoritative server collision behavior to reduce airborne reconciliation jitter.
    constexpr bool kClientBlockOnMissingCollisionChunk = true;
    constexpr float kCollisionSkin = 0.001f;
    constexpr bool kPlayerModelYawInvert = true;
    constexpr float kPlayerModelYawOffsetDeg = 0.0f;

    float NormalizeYawDegrees(float yawDegrees) {
        if (!std::isfinite(yawDegrees)) {
            return 0.0f;
        }
        float y = std::fmod(yawDegrees, 360.0f);
        if (y >= 180.0f) y -= 360.0f;
        if (y < -180.0f) y += 360.0f;
        return y;
    }

    float ToModelYawDegrees(float lookYawDegrees) {
        const float signedYaw = kPlayerModelYawInvert ? -lookYawDegrees : lookYawDegrees;
        return NormalizeYawDegrees(signedYaw + kPlayerModelYawOffsetDeg);
    }

    inline int ifloor(float v) {
        return static_cast<int>(std::floor(v));
    }

    constexpr bool kEnableHitboxDiagnostics = true;

    bool IsImGuiTextInputActive() {
        if (ImGui::GetCurrentContext() == nullptr) {
            return false;
        }
        const ImGuiIO& io = ImGui::GetIO();
        return io.WantTextInput || io.WantCaptureKeyboard;
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

    const char* HitRegionName(HitRegion region) {
        switch (region) {
        case HitRegion::Head: return "Head";
        case HitRegion::Body: return "Body";
        case HitRegion::Legs: return "Legs";
        default: return "Unknown";
        }
    }

    void LogHitboxes(const char* source, const std::vector<Hitbox>& hitboxes, float playerHeight, float playerRadius) {
        if (!kEnableHitboxDiagnostics) {
            return;
        }
        std::cout
            << "[hitbox/client] source=" << source
            << " count=" << hitboxes.size()
            << " collisionHeight=" << playerHeight
            << " collisionRadius=" << playerRadius
            << "\n";
        for (size_t i = 0; i < hitboxes.size(); ++i) {
            const Hitbox& hb = hitboxes[i];
            std::cout
                << "  [" << i << "] region=" << HitRegionName(hb.region)
                << " min=(" << hb.min.x << "," << hb.min.y << "," << hb.min.z << ")"
                << " max=(" << hb.max.x << "," << hb.max.y << "," << hb.max.z << ")"
                << "\n";
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

    uint8_t CacheCodeFromRegion(HitRegion region) {
        switch (region) {
        case HitRegion::Legs: return 0;
        case HitRegion::Body: return 1;
        case HitRegion::Head: return 2;
        default: return 1;
        }
    }

    uint8_t CacheCodeFromModelRegion(ModelRegion region) {
        switch (region) {
        case ModelRegion::Legs: return 0;
        case ModelRegion::Body: return 1;
        case ModelRegion::Head: return 2;
        default: return 1;
        }
    }

    std::vector<Hitbox> BuildHitboxesFromModel(const Model& model, float playerHeight, float playerRadius) {
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
        if (xMax <= xMin) xMax = xMin + 1e-4f;
        if (zMax <= zMin) zMax = zMin + 1e-4f;

        float legsTop = targetHeight * 0.40f;
        float headBottom = targetHeight * 0.72f;
        const ModelRegionAabb& legsRegion = model.getLocalRegionAabb(ModelRegion::Legs);
        const ModelRegionAabb& headRegion = model.getLocalRegionAabb(ModelRegion::Head);
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
            if (hb.max.y <= hb.min.y) hb.max.y = hb.min.y + 1e-4f;
            hb.region = region;
            out.push_back(hb);
        };

        pushBox(0.0f, legsTop, HitRegion::Legs);
        pushBox(legsTop, headBottom, HitRegion::Body);
        pushBox(headBottom, targetHeight, HitRegion::Head);
        return out;
    }

    std::vector<Shared::MeshHitCache::TriangleRecord> BuildMeshTrianglesFromModel(const Model& model, float playerHeight) {
        std::vector<Shared::MeshHitCache::TriangleRecord> out;
        const float targetHeight = std::max(playerHeight, 0.01f);
        const float modelMinY = model.getLocalMinY();
        const float modelHeight = model.getLocalHeight();
        const float uniformScale = targetHeight / std::max(modelHeight, 1e-4f);
        const glm::vec3 offset(0.0f, -modelMinY * uniformScale, 0.0f);

        const std::vector<ModelLocalTriangle>& tris = model.getLocalTriangles();
        out.reserve(tris.size());
        for (const ModelLocalTriangle& tri : tris) {
            Shared::MeshHitCache::TriangleRecord rec;
            const glm::vec3 a = tri.a * uniformScale + offset;
            const glm::vec3 b = tri.b * uniformScale + offset;
            const glm::vec3 c = tri.c * uniformScale + offset;
            rec.ax = a.x; rec.ay = a.y; rec.az = a.z;
            rec.bx = b.x; rec.by = b.y; rec.bz = b.z;
            rec.cx = c.x; rec.cy = c.y; rec.cz = c.z;
            rec.region = CacheCodeFromModelRegion(tri.region);
            out.push_back(rec);
        }
        return out;
    }

    bool SaveHitboxCache(const std::vector<Hitbox>& hitboxes, float playerHeight, float playerRadius) {
        std::vector<Shared::HitboxCache::Record> records;
        records.reserve(hitboxes.size());
        for (const Hitbox& hb : hitboxes) {
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
        const bool ok = Shared::HitboxCache::Save(SharedHitboxCachePath(), playerHeight, playerRadius, records);
        if (kEnableHitboxDiagnostics) {
            std::cout
                << "[hitbox/client] cache_write path=" << SharedHitboxCachePath()
                << " status=" << (ok ? "ok" : "failed")
                << " count=" << records.size()
                << "\n";
        }
        return ok;
    }

    bool SaveMeshHitCache(const std::vector<Shared::MeshHitCache::TriangleRecord>& triangles, float playerHeight) {
        const bool ok = Shared::MeshHitCache::Save(SharedMeshHitCachePath(), playerHeight, triangles);
        if (kEnableHitboxDiagnostics) {
            std::cout
                << "[hitbox/client] mesh_cache_write path=" << SharedMeshHitCachePath()
                << " status=" << (ok ? "ok" : "failed")
                << " triangles=" << triangles.size()
                << "\n";
        }
        return ok;
    }

    bool LoadHitboxCache(std::vector<Hitbox>& out, float playerHeight, float playerRadius) {
        float referenceHeight = 0.0f;
        float referenceRadius = 0.0f;
        std::vector<Shared::HitboxCache::Record> records;
        if (!Shared::HitboxCache::Load(SharedHitboxCachePath(), referenceHeight, referenceRadius, records)) {
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
        for (const Shared::HitboxCache::Record& rec : records) {
            Hitbox hb;
            hb.min = glm::vec3(rec.minX * sx, rec.minY * sy, rec.minZ * sz);
            hb.max = glm::vec3(rec.maxX * sx, rec.maxY * sy, rec.maxZ * sz);
            hb.region = RegionFromCacheCode(rec.region);
            out.push_back(hb);
        }
        if (kEnableHitboxDiagnostics) {
            std::cout
                << "[hitbox/client] cache_load path=" << SharedHitboxCachePath()
                << " records=" << records.size()
                << " refHeight=" << referenceHeight
                << " refRadius=" << referenceRadius
                << " scale=(" << sx << "," << sy << "," << sz << ")"
                << "\n";
        }
        return true;
    }
}


Player::Player(const glm::vec3& startPos, ChunkManager& inChunkManager, const std::string& playerModelPath)
    : position(startPos),
    velocity(0.0f),
    front(0.0f, 0.0f, -1.0f),
    currentFov(walkFov),
    camera(startPos),
    chunkManager(inChunkManager),
    onGround(false),
    yaw(-90.0f),
    pitch(0.0f)
{
    const auto& movement = movementSettings();
    moveSpeed = movement.walkSpeed;
    runSpeed = movement.sprintSpeed;
    jumpVelocity = movement.jumpVelocity;
    playerHeight = movement.collisionHeight;
    playerRadius = movement.collisionRadius;

    // load model
    try {
        playerModel = std::make_shared<Model>(playerModelPath);
    }
    catch (const std::exception& e) {
        std::cerr << "Model load exception: " << e.what() << "\n";
        playerModel.reset();
    }

    if (!LoadHitboxCache(m_hitboxes, playerHeight, playerRadius)) {
        if (playerModel) {
            m_hitboxes = BuildHitboxesFromModel(*playerModel, playerHeight, playerRadius);
            if (!m_hitboxes.empty()) {
                if (!SaveHitboxCache(m_hitboxes, playerHeight, playerRadius)) {
                    std::cerr << "Warning: failed to write hitbox cache: " << SharedHitboxCachePath() << "\n";
                }
                LogHitboxes("mesh_derived", m_hitboxes, playerHeight, playerRadius);
            }
        }
    }
    else {
        LogHitboxes("cache", m_hitboxes, playerHeight, playerRadius);
    }
    if (m_hitboxes.empty()) {
        m_hitboxes = HitboxManager::buildBlockyHitboxes(playerHeight, playerRadius, playerRadius, true);
        LogHitboxes("fallback_blocky", m_hitboxes, playerHeight, playerRadius);
    }

    if (playerModel) {
        const auto triangles = BuildMeshTrianglesFromModel(*playerModel, playerHeight);
        if (!triangles.empty()) {
            if (!SaveMeshHitCache(triangles, playerHeight)) {
                std::cerr << "Warning: failed to write mesh hit cache: " << SharedMeshHitCachePath() << "\n";
            }
        }
    }

    syncCameraToBody();

    try {
        const std::string playerVertPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.vert").generic_string();
        const std::string playerFragPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.frag").generic_string();
        playerShader = std::make_shared<Shader>(playerVertPath.c_str(), playerFragPath.c_str());
    }
    catch (const std::exception& e) {
        std::cerr << "Shader load exception: " << e.what() << "\n";
        playerShader.reset();
    }

    // sanity checks
    if (!playerModel)  std::cerr << "Warning: playerModel not loaded\n";
    if (!playerShader) std::cerr << "Warning: playerShader not created\n";
}


const std::vector<Hitbox>& Player::getHitboxes() const noexcept {
    return m_hitboxes;
}

const glm::mat4& Player::getModelMatrix() const noexcept {
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
    const auto& movement = movementSettings();
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

bool Player::checkCollision(const glm::vec3& pos) const {
    if (flyMode) return false; // [FLY MODE] disables collisions

    // Shrink AABB slightly so face-touching at block boundaries is not treated as penetration.
    float minX = pos.x - playerRadius + kCollisionSkin;
    float maxX = pos.x + playerRadius - kCollisionSkin;
    float minY = pos.y + kCollisionSkin;
    float maxY = pos.y + playerHeight - kCollisionSkin;
    float minZ = pos.z - playerRadius + kCollisionSkin;
    float maxZ = pos.z + playerRadius - kCollisionSkin;

    int ix0 = ifloor(minX);
    int iy0 = ifloor(minY);
    int iz0 = ifloor(minZ);
    int ix1 = ifloor(maxX);
    int iy1 = ifloor(maxY);
    int iz1 = ifloor(maxZ);

    for (int x = ix0; x <= ix1; ++x) {
        for (int y = iy0; y <= iy1; ++y) {
            for (int z = iz0; z <= iz1; ++z) {
                const glm::ivec3 worldPos(x, y, z);
                const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
                if (chunkManager.inBounds(chunkPos) && !chunkManager.hasChunkLoaded(chunkPos)) {
                    if (kClientBlockOnMissingCollisionChunk) {
                        // Optional conservative mode near stream edges.
                        return true;
                    }
                    continue;
                }
                if (chunkManager.getBlockGlobal(x, y, z) != BlockID::Air) {
                    return true;
                }
            }
        }
    }
    return false;
}


void Player::moveAndCollide(const glm::vec3& delta, bool allowStepUp, float* outStepUpHeight) {
    Shared::Movement::State state;
    state.position = position;
    state.velocity = velocity;
    state.onGround = onGround;
    state.flyMode = flyMode;
    state.jumpPressedLastTick = m_jumpPressedLastTick;
    state.timeSinceGrounded = m_timeSinceGrounded;
    state.jumpBufferTimer = m_jumpBufferTimer;

    const float stepUpHeight = Shared::Movement::MoveAndCollide(
        state,
        delta,
        movementSettings(),
        allowStepUp,
        [this](const glm::vec3& testPos) { return checkCollision(testPos); }
    );

    position = state.position;
    velocity = state.velocity;
    onGround = state.onGround;

    if (outStepUpHeight != nullptr) {
        *outStepUpHeight = stepUpHeight;
    }
}

void Player::setPosition(const glm::vec3& p) noexcept {
    position = p;
    syncCameraToBody();
}

Player::SimulationState Player::captureSimulationState() const noexcept {
    SimulationState state;
    state.position = position;
    state.velocity = velocity;
    state.front = front;
    state.yaw = yaw;
    state.pitch = pitch;
    state.onGround = onGround;
    state.flyMode = flyMode;
    state.jumpPressedLastTick = m_jumpPressedLastTick;
    state.timeSinceGrounded = m_timeSinceGrounded;
    state.jumpBufferTimer = m_jumpBufferTimer;
    state.currentFov = currentFov;
    state.stepUpVisualOffset = m_stepUpVisualOffset;
    return state;
}

void Player::restoreSimulationState(const SimulationState& state) noexcept {
    position = state.position;
    velocity = state.velocity;
    front = state.front;
    yaw = static_cast<double>(NormalizeYawDegrees(static_cast<float>(state.yaw)));
    pitch = glm::clamp(state.pitch, -89.0f, 89.0f);
    onGround = state.onGround;
    flyMode = m_flyModeAllowed && state.flyMode;
    m_jumpPressedLastTick = state.jumpPressedLastTick;
    m_timeSinceGrounded = state.timeSinceGrounded;
    m_jumpBufferTimer = state.jumpBufferTimer;
    currentFov = state.currentFov;
    m_stepUpVisualOffset = std::max(0.0f, state.stepUpVisualOffset);

    camera.updateRotation(static_cast<float>(yaw), pitch);
    front = camera.front;
    syncCameraToBody();
}

void Player::setFlyModeAllowed(bool allowed) noexcept {
    if (m_flyModeAllowed == allowed) {
        return;
    }
    m_flyModeAllowed = allowed;
    if (!m_flyModeAllowed) {
        flyMode = false;
    }
}

void Player::simulateFromNetworkInput(const NetworkInputState& input, double deltaTime, bool updateFov) {
    simulateMovement(input, static_cast<float>(deltaTime), updateFov);
}

void Player::simulateMovement(const NetworkInputState& input, float dt, bool updateFov) {
    const auto& movement = movementSettings();

    yaw = static_cast<double>(NormalizeYawDegrees(input.yaw));
    pitch = glm::clamp(input.pitch, -89.0f, 89.0f);
    camera.updateRotation(static_cast<float>(yaw), pitch);
    front = camera.front;

    const uint8_t flags = input.flags;
    const bool sprint = (flags & kPlayerInputFlagSprint) != 0;

    if (updateFov) {
        const float targetFov = sprint ? runningFov * runningFovMultiplier : walkFov;
        const float fovSmoothSpeed = 20.0f;
        currentFov += (targetFov - currentFov) * fovSmoothSpeed * dt;
    }

    Shared::Movement::State state;
    state.position = position;
    state.velocity = velocity;
    state.onGround = onGround;
    state.flyMode = flyMode;
    state.jumpPressedLastTick = m_jumpPressedLastTick;
    state.timeSinceGrounded = m_timeSinceGrounded;
    state.jumpBufferTimer = m_jumpBufferTimer;

    Shared::Movement::InputState simInput;
    simInput.moveX = input.moveX;
    simInput.moveZ = input.moveZ;
    simInput.flags = input.flags;
    simInput.flyMode = input.flyMode;

    Shared::Movement::Options simOptions;
    simOptions.allowFlyMode = m_flyModeAllowed;
    simOptions.allowStepUp = true;
    simOptions.requireSprintForStepUp = true;

    float steppedHeight = 0.0f;
    Shared::Movement::Simulate(
        state,
        simInput,
        dt,
        movement,
        simOptions,
        [this](const glm::vec3& testPos) { return checkCollision(testPos); },
        &steppedHeight
    );

    position = state.position;
    velocity = state.velocity;
    onGround = state.onGround;
    flyMode = state.flyMode;
    m_jumpPressedLastTick = state.jumpPressedLastTick;
    m_timeSinceGrounded = state.timeSinceGrounded;
    m_jumpBufferTimer = state.jumpBufferTimer;

    if (steppedHeight > 0.0f) {
        // Apply only a fraction of physical step-up for a gentler camera response.
        // Keep the offset stable during consecutive step ticks to avoid visual chatter.
        const float visualStep = std::min(steppedHeight * m_stepUpVisualScale, m_stepUpOffsetMax);
        m_stepUpVisualOffset = std::max(m_stepUpVisualOffset, visualStep);
    }
    else if (!onGround && velocity.y < 0.0f) {
        // While falling, bleed step-up compensation quickly instead of hard-resetting it.
        const float keep = std::exp(-18.0f * dt);
        m_stepUpVisualOffset *= std::clamp(keep, 0.0f, 1.0f);
        if (m_stepUpVisualOffset < 1e-4f) {
            m_stepUpVisualOffset = 0.0f;
        }
    }
    else {
        decayStepUpOffset(dt);
    }

    syncCameraToBody();
}




// update (called each frame)
void Player::update(GLFWwindow* window, double deltaTime) {
    static bool f8PressedLast = false;
    const bool allowGameplayInput = !GameData::cursorEnabled && !IsImGuiTextInputActive();
    const bool f8Pressed = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_F8) == GLFW_PRESS);
    if (m_flyModeAllowed && f8Pressed && !f8PressedLast) {
        flyMode = !flyMode;
        std::cout << (flyMode ? "Fly mode ON\n" : "Fly mode OFF\n");
        velocity = glm::vec3(0.0f);
        onGround = false;
        m_jumpPressedLastTick = false;
        m_timeSinceGrounded = 0.0f;
        m_jumpBufferTimer = 0.0f;
    }
    else if (!m_flyModeAllowed) {
        flyMode = false;
    }
    f8PressedLast = f8Pressed;

    const bool keyW = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);
    const bool keyS = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS);
    const bool keyA = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS);
    const bool keyD = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS);
    const bool keyShift = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
    const bool keySpace = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
    const bool keyCtrl = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS);

    const glm::vec2 localMove(
        (keyD ? 1.0f : 0.0f) - (keyA ? 1.0f : 0.0f),
        (keyW ? 1.0f : 0.0f) - (keyS ? 1.0f : 0.0f)
    );
    glm::vec2 localMoveNormalized = localMove;
    if (glm::length(localMoveNormalized) > 1.0f) {
        localMoveNormalized = glm::normalize(localMoveNormalized);
    }
    const float yawRad = glm::radians(static_cast<float>(yaw));
    const glm::vec2 forward2D(std::cos(yawRad), std::sin(yawRad));
    const glm::vec2 right2D(-forward2D.y, forward2D.x);
    const glm::vec2 worldMove = right2D * localMoveNormalized.x + forward2D * localMoveNormalized.y;
    m_networkInput.moveX = worldMove.x;
    m_networkInput.moveZ = worldMove.y;
    m_networkInput.yaw = NormalizeYawDegrees(static_cast<float>(yaw));
    m_networkInput.pitch = pitch;
    m_networkInput.flyMode = flyMode;
    m_networkInput.flags = 0;
    if (keyW) m_networkInput.flags |= kPlayerInputFlagForward;
    if (keyS) m_networkInput.flags |= kPlayerInputFlagBackward;
    if (keyA) m_networkInput.flags |= kPlayerInputFlagLeft;
    if (keyD) m_networkInput.flags |= kPlayerInputFlagRight;
    if (keySpace) m_networkInput.flags |= kPlayerInputFlagJump;
    if (keyShift) m_networkInput.flags |= kPlayerInputFlagSprint;
    if (flyMode && keySpace) m_networkInput.flags |= kPlayerInputFlagFlyUp;
    if (flyMode && keyCtrl) m_networkInput.flags |= kPlayerInputFlagFlyDown;

    if (deltaTime > 0.0) {
        simulateMovement(m_networkInput, static_cast<float>(deltaTime), true);
    }
}




NetworkInputState Player::captureCurrentInput(GLFWwindow* window) const noexcept {
    NetworkInputState input;
    
    const bool allowGameplayInput = !GameData::cursorEnabled && !IsImGuiTextInputActive();
    
    const bool keyW = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);
    const bool keyS = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS);
    const bool keyA = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS);
    const bool keyD = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS);
    const bool keyShift = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
    const bool keySpace = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
    const bool keyCtrl = allowGameplayInput && (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS);

    const glm::vec2 localMove(
        (keyD ? 1.0f : 0.0f) - (keyA ? 1.0f : 0.0f),
        (keyW ? 1.0f : 0.0f) - (keyS ? 1.0f : 0.0f)
    );
    glm::vec2 localMoveNormalized = localMove;
    if (glm::length(localMoveNormalized) > 1.0f) {
        localMoveNormalized = glm::normalize(localMoveNormalized);
    }
    const float yawRad = glm::radians(static_cast<float>(yaw));
    const glm::vec2 forward2D(std::cos(yawRad), std::sin(yawRad));
    const glm::vec2 right2D(-forward2D.y, forward2D.x);
    const glm::vec2 worldMove = right2D * localMoveNormalized.x + forward2D * localMoveNormalized.y;
    input.moveX = worldMove.x;
    input.moveZ = worldMove.y;
    input.yaw = NormalizeYawDegrees(static_cast<float>(yaw));
    input.pitch = pitch;
    input.flyMode = flyMode;
    input.flags = 0;
    if (keyW) input.flags |= kPlayerInputFlagForward;
    if (keyS) input.flags |= kPlayerInputFlagBackward;
    if (keyA) input.flags |= kPlayerInputFlagLeft;
    if (keyD) input.flags |= kPlayerInputFlagRight;
    if (keySpace) input.flags |= kPlayerInputFlagJump;
    if (keyShift) input.flags |= kPlayerInputFlagSprint;
    if (flyMode && keySpace) input.flags |= kPlayerInputFlagFlyUp;
    if (flyMode && keyCtrl) input.flags |= kPlayerInputFlagFlyDown;
    
    return input;
}



void Player::setConnectedPlayers(const std::unordered_map<PlayerID, PlayerState>& players) {
    m_remotePlayerTargets.clear();
    m_remotePlayerTargets.reserve(players.size());

    for (const auto& [id, state] : players) {
        auto currentIt = connectedPlayers.find(id);
        if (currentIt == connectedPlayers.end()) {
            connectedPlayers.emplace(id, state);
        }
        m_remotePlayerTargets.emplace(id, state);
    }

    for (auto it = connectedPlayers.begin(); it != connectedPlayers.end();) {
        if (m_remotePlayerTargets.find(it->first) == m_remotePlayerTargets.end()) {
            it = connectedPlayers.erase(it);
        }
        else {
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

    for (auto& [id, current] : connectedPlayers) {
        auto targetIt = m_remotePlayerTargets.find(id);
        if (targetIt == m_remotePlayerTargets.end()) {
            continue;
        }
        current = targetIt->second;
    }
}

void Player::processMouse(bool dbgCam, double xpos, double ypos) noexcept {
    if (dbgCam) return;

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
        return;
    }

    double xoffset = xpos - lastX;
    double yoffset = ypos - lastY;
    lastX = xpos;
    lastY = ypos;

    xoffset *= mouseSensitivity;
    yoffset *= mouseSensitivity;

    yaw = static_cast<double>(NormalizeYawDegrees(static_cast<float>(yaw + xoffset)));
    pitch -= static_cast<float>(yoffset);
    pitch = glm::clamp(pitch, -89.0f, 89.0f);

    camera.updateRotation(yaw, pitch);

    // keep front consistent and update model matrix orientation
    front = camera.front;
    updateModelMatrix();
}

bool Player::isGrounded() const noexcept {
    return onGround;
}

glm::mat4 Player::getViewMatrix() const noexcept {
    return camera.getViewMatrix();
}



void Player::renderRemotePlayers(const glm::mat4& viewMat, const glm::mat4& projMat, const glm::vec3& lightDir, const glm::vec3& lightColor, const glm::vec3& ambientColor) const {
    if (!playerShader || !playerModel || connectedPlayers.empty()) {
        return;
    }

    glDisable(GL_CULL_FACE);

    playerShader->use();


    playerShader->setInt("diffuseTexture", 0);

    playerShader->setVec3("lightDir", lightDir);//normalized
    playerShader->setVec3("lightColor", lightColor);
    playerShader->setVec3("ambientColor", ambientColor);

    // also set the view/projection matrices
    playerShader->setMat4("view", viewMat);
    playerShader->setMat4("projection", projMat);

    const glm::vec3 modelSize = playerModel->getLocalSize();
    const float modelMinY = playerModel->getLocalMinY();
    const float uniformFitToCollision = std::max(playerHeight, 0.01f) / std::max(modelSize.y, 1e-4f);

    for (const auto& [id, state] : connectedPlayers) {
        (void)id;
        const glm::vec3 scaled = state.scale * uniformFitToCollision;
        const glm::vec3 anchoredPos = state.position + glm::vec3(0.0f, -modelMinY * scaled.y, 0.0f);
        playerModel->draw(anchoredPos, state.rotation, scaled, *playerShader);
    }

    glEnable(GL_CULL_FACE);

}






//client side for now
void Player::placeBlock(BlockMode blockMode) {
    Ray ray(camera.position, camera.front);
    if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hit) {


    }
}



void Player::breakBlock() {
    Ray ray(camera.position, camera.front);
    if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hit) {
        glm::ivec3 hitBlock = rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hitBlockWorld;

        chunkManager.playerBreakBlockAt(hitBlock);
    }
}

