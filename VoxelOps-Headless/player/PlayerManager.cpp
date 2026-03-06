#include "PlayerManager.hpp"
#include "../../Shared/network/Packets.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/player/MovementSimulation.hpp"
#include "../graphics/ChunkManager.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <chrono>
#include <atomic>
#include <array>
#include <cstring>
#include <limits>
#include <glm/glm.hpp>

namespace {
constexpr size_t kMaxBufferedInputs = 256;
constexpr int64_t kSlowPlayerManagerUpdateUs = 4000;
std::atomic<uint64_t> g_playerManagerSlowUpdateCount{ 0 };
constexpr bool kServerBlockOnMissingCollisionChunk = true;
std::atomic<uint64_t> g_missingChunkCollisionCount{ 0 };
std::atomic<bool> g_enablePlayerManagerPerfDiagnostics{ true };
std::atomic<bool> g_enableMissingChunkCollisionDiagnostics{ true };
constexpr size_t kPlayerSnapshotEntrySize = 8 + (8 * 4) + 3 + 2 + 4 + 1 + 4;
const std::array<glm::vec3, 9> kRespawnCandidates{ {
    glm::vec3(0.0f, 60.0f, 0.0f),
    glm::vec3(14.0f, 60.0f, 14.0f),
    glm::vec3(-14.0f, 60.0f, 14.0f),
    glm::vec3(14.0f, 60.0f, -14.0f),
    glm::vec3(-14.0f, 60.0f, -14.0f),
    glm::vec3(24.0f, 60.0f, 0.0f),
    glm::vec3(-24.0f, 60.0f, 0.0f),
    glm::vec3(0.0f, 60.0f, 24.0f),
    glm::vec3(0.0f, 60.0f, -24.0f)
} };

inline const Shared::PlayerData::MovementSettings& movementSettings() {
    return Shared::PlayerData::GetMovementSettings();
}

inline bool IsNewerU32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline float NormalizeYawDegrees(float yawDegrees) {
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f) y -= 360.0f;
    if (y < -180.0f) y += 360.0f;
    return y;
}

inline void AppendU8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}

inline void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

inline void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

inline void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

inline void AppendF32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32(out, bits);
}

std::vector<uint8_t> SerializePlayerSnapshotsPayload(const std::vector<PlayerSnapshot>& players) {
    std::vector<uint8_t> out;
    out.reserve(4 + players.size() * kPlayerSnapshotEntrySize);
    AppendU32(out, static_cast<uint32_t>(players.size()));
    for (const PlayerSnapshot& p : players) {
        AppendU64(out, p.id);
        AppendF32(out, p.px);
        AppendF32(out, p.py);
        AppendF32(out, p.pz);
        AppendF32(out, p.vx);
        AppendF32(out, p.vy);
        AppendF32(out, p.vz);
        AppendF32(out, p.yaw);
        AppendF32(out, p.pitch);
        AppendU8(out, p.onGround);
        AppendU8(out, p.flyMode);
        AppendU8(out, p.allowFlyMode);
        AppendU16(out, p.weaponId);
        AppendF32(out, p.health);
        AppendU8(out, p.isAlive);
        AppendF32(out, p.respawnSeconds);
    }
    return out;
}

std::vector<uint8_t> BuildSnapshotFrameBytes(
    uint32_t serverTick,
    PlayerID selfPlayerId,
    uint32_t lastProcessedInputSequence,
    const std::vector<uint8_t>& playersPayload
) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 8 + 4 + playersPayload.size());
    AppendU8(out, static_cast<uint8_t>(PacketType::PlayerSnapshot));
    AppendU32(out, serverTick);
    AppendU64(out, selfPlayerId);
    AppendU32(out, lastProcessedInputSequence);
    out.insert(out.end(), playersPayload.begin(), playersPayload.end());
    return out;
}
}

PlayerManager::PlayerManager() = default;

void PlayerManager::SetDebugLoggingEnabled(bool enabled) {
    g_enablePlayerManagerPerfDiagnostics.store(enabled, std::memory_order_release);
    g_enableMissingChunkCollisionDiagnostics.store(enabled, std::memory_order_release);
}

bool PlayerManager::IsDebugLoggingEnabled() {
    return g_enablePlayerManagerPerfDiagnostics.load(std::memory_order_acquire) ||
        g_enableMissingChunkCollisionDiagnostics.load(std::memory_order_acquire);
}

glm::vec3 PlayerManager::chooseRespawnPositionLocked(PlayerID respawningId) const {
    if (kRespawnCandidates.empty()) {
        return glm::vec3(0.0f, 60.0f, 0.0f);
    }

    float bestScore = -1.0f;
    glm::vec3 bestPosition = kRespawnCandidates.front();
    bool foundAnyAliveOpponent = false;

    for (const glm::vec3& candidate : kRespawnCandidates) {
        float nearestAliveDistSq = std::numeric_limits<float>::max();
        bool hasAliveOpponent = false;
        for (const auto& kv : playersById) {
            if (kv.first == respawningId) {
                continue;
            }
            const ServerPlayer& other = kv.second;
            if (!other.isAlive) {
                continue;
            }
            const glm::vec3 delta = candidate - other.position;
            const float distSq = glm::dot(delta, delta);
            nearestAliveDistSq = std::min(nearestAliveDistSq, distSq);
            hasAliveOpponent = true;
        }

        if (!hasAliveOpponent) {
            if (!foundAnyAliveOpponent) {
                return candidate;
            }
            continue;
        }

        foundAnyAliveOpponent = true;
        if (nearestAliveDistSq > bestScore) {
            bestScore = nearestAliveDistSq;
            bestPosition = candidate;
        }
    }

    return bestPosition;
}

void PlayerManager::respawnPlayerLocked(ServerPlayer& player, const glm::vec3& position) {
    player.position = position;
    player.velocity = glm::vec3(0.0f);
    player.onGround = false;
    player.flyMode = false;
    player.activeInputFlags = 0;
    player.moveX = 0.0f;
    player.moveZ = 0.0f;
    player.jumpPressedLastTick = false;
    player.timeSinceGrounded = 0.0f;
    player.jumpBufferTimer = 0.0f;
    player.pendingInputs.clear();
    player.health = player.maxHealth;
    player.isAlive = true;
    player.respawnAt = Clock::time_point{};
    player.pendingRespawnRequest = false;
}

PlayerID PlayerManager::addPlayerInternal() {
    return nextId.fetch_add(1, std::memory_order_relaxed);
}

PlayerID PlayerManager::onPlayerConnect(std::shared_ptr<ConnectionHandle> conn, const glm::vec3& spawnPos) {
    std::lock_guard<std::mutex> lock(mtx);
    PlayerID id = addPlayerInternal();
    const auto now = Clock::now();
    ServerPlayer p;
    p.id = id;
    p.position = spawnPos;
    p.velocity = glm::vec3(0.0f);
    p.height = movementSettings().collisionHeight;
    p.radius = movementSettings().collisionRadius;
    p.health = p.maxHealth;
    p.isAlive = true;
    p.respawnAt = Clock::time_point{};
    p.pendingRespawnRequest = false;
    p.lastHeartbeat = now;
    p.lastInputReceived = now;
    p.conn = conn;

    playersOrder.push_back(id);
    auto it = std::prev(playersOrder.end());
    p.orderIt = it;

    playersById.emplace(id, std::move(p));
    std::cout << "Player " << id << " connected\n";
    return id;
}

bool PlayerManager::removePlayer(PlayerID id) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return false;
    playersOrder.erase(it->second.orderIt); // O(1)
    playersById.erase(it);
    std::cout << "Player " << id << " removed\n";
    return true;
}

bool PlayerManager::touchHeartbeat(PlayerID id) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return false;
    it->second.lastHeartbeat = Clock::now();
    return true;
}

bool PlayerManager::enqueuePlayerInput(PlayerID id, const PlayerInput& input) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return false;

    ServerPlayer& p = it->second;
    if (!IsNewerU32(input.sequenceNumber, p.lastProcessedInputSequence)) {
        return true;
    }
    if (!p.pendingInputs.empty() && !IsNewerU32(input.sequenceNumber, p.pendingInputs.back().sequenceNumber)) {
        return true;
    }

    if (p.pendingInputs.size() >= kMaxBufferedInputs) {
        p.pendingInputs.pop_front();
    }
    p.pendingInputs.push_back(input);
    const auto now = Clock::now();
    p.lastHeartbeat = now;
    p.lastInputReceived = now;
    return true;
}

bool PlayerManager::setFlyModeAllowed(PlayerID id, bool allowed) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return false;

    ServerPlayer& p = it->second;
    p.allowFlyMode = allowed;
    if (!allowed) {
        p.flyMode = false;
        p.activeInputFlags &= static_cast<uint8_t>(~(kPlayerInputFlagFlyUp | kPlayerInputFlagFlyDown));
    }
    return true;
}

bool PlayerManager::setEquippedWeapon(PlayerID id, uint16_t weaponId) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return false;

    it->second.equippedWeaponId = weaponId;
    return true;
}

void PlayerManager::update(double deltaSeconds, ChunkManager& chunkManager) {
    const auto updateStart = std::chrono::steady_clock::now();
    std::vector<PlayerID> toRemove;
    size_t playerCountForLog = 0;
    {
        std::lock_guard<std::mutex> lock(mtx);
        playerCountForLog = playersById.size();
        const auto now = Clock::now();
        for (auto& kv : playersById) {
            ServerPlayer& player = kv.second;
            if (player.isAlive) {
                simulatePhysicsFor(player, deltaSeconds, chunkManager);
            }
            else {
                while (!player.pendingInputs.empty()) {
                    const PlayerInput cmd = player.pendingInputs.front();
                    player.pendingInputs.pop_front();
                    if (!IsNewerU32(cmd.sequenceNumber, player.lastProcessedInputSequence)) {
                        continue;
                    }
                    player.lastProcessedInputSequence = cmd.sequenceNumber;
                    player.yaw = NormalizeYawDegrees(std::isfinite(cmd.yaw) ? cmd.yaw : 0.0f);
                    player.pitch = std::isfinite(cmd.pitch) ? cmd.pitch : 0.0f;
                }

                player.activeInputFlags = 0;
                player.moveX = 0.0f;
                player.moveZ = 0.0f;
                player.flyMode = false;
                player.velocity = glm::vec3(0.0f);
                player.onGround = false;
                player.jumpPressedLastTick = false;
                player.timeSinceGrounded = 0.0f;
                player.jumpBufferTimer = 0.0f;

                if (
                    player.pendingRespawnRequest &&
                    player.respawnAt != Clock::time_point{} &&
                    now >= player.respawnAt
                ) {
                    const glm::vec3 respawnPos = chooseRespawnPositionLocked(player.id);
                    respawnPlayerLocked(player, respawnPos);
                    player.lastInputReceived = now;
                }
            }

            if (now - player.lastHeartbeat > heartbeatTimeout) {
                toRemove.push_back(kv.first);
            }
        }

        for (auto id : toRemove) {
            auto it = playersById.find(id);
            if (it != playersById.end()) {
                playersOrder.erase(it->second.orderIt);
                playersById.erase(it);
                std::cout << "Player " << id << " timed out and removed\n";
            }
        }
    }
    const int64_t updateUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - updateStart
    ).count();
    if (g_enablePlayerManagerPerfDiagnostics.load(std::memory_order_acquire) && updateUs >= kSlowPlayerManagerUpdateUs) {
        const uint64_t count = g_playerManagerSlowUpdateCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 40 || (count % 200) == 0) {
            std::cerr
                << "[perf/player-manager] slow update us=" << updateUs
                << " players=" << playerCountForLog
                << " count=" << count << "\n";
        }
    }
}

bool PlayerManager::checkCollision(const ServerPlayer& p, const glm::vec3& pos, ChunkManager& chunkManager) const {
    if (p.flyMode) return false;
    const ChunkManager::AabbCollisionQueryResult query = chunkManager.queryAabbCollision(
        pos,
        p.radius,
        p.height,
        kServerBlockOnMissingCollisionChunk
    );
    if (query.missingChunk && g_enableMissingChunkCollisionDiagnostics.load(std::memory_order_acquire)) {
        const uint64_t count =
            g_missingChunkCollisionCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 40 || (count % 200) == 0) {
            std::cerr
                << "[perf/player-manager] missing collision chunk=("
                << query.firstMissingChunk.x << "," << query.firstMissingChunk.y << "," << query.firstMissingChunk.z << ")"
                << " playerPos=("
                << p.position.x << "," << p.position.y << "," << p.position.z << ")"
                << " count=" << count << "\n";
        }
    }
    return query.collided;
}

void PlayerManager::moveAndCollide(
    ServerPlayer& p,
    const glm::vec3& delta,
    ChunkManager& chunkManager,
    bool allowStepUp
) {
    Shared::Movement::State state;
    state.position = p.position;
    state.velocity = p.velocity;
    state.onGround = p.onGround;
    state.flyMode = p.flyMode;
    state.jumpPressedLastTick = p.jumpPressedLastTick;
    state.timeSinceGrounded = p.timeSinceGrounded;
    state.jumpBufferTimer = p.jumpBufferTimer;

    Shared::Movement::MoveAndCollide(
        state,
        delta,
        movementSettings(),
        allowStepUp,
        [&p, &chunkManager, this](const glm::vec3& testPos) {
            return checkCollision(p, testPos, chunkManager);
        }
    );

    p.position = state.position;
    p.velocity = state.velocity;
    p.onGround = state.onGround;
    p.jumpPressedLastTick = state.jumpPressedLastTick;
    p.timeSinceGrounded = state.timeSinceGrounded;
    p.jumpBufferTimer = state.jumpBufferTimer;
}

void PlayerManager::simulatePhysicsFor(ServerPlayer& p, double dt, ChunkManager& chunkManager) {
    const auto& movement = movementSettings();
    constexpr uint8_t kContinuousMoveFlags =
        kPlayerInputFlagForward |
        kPlayerInputFlagBackward |
        kPlayerInputFlagLeft |
        kPlayerInputFlagRight |
        kPlayerInputFlagSprint |
        kPlayerInputFlagFlyUp |
        kPlayerInputFlagFlyDown;

    // Consume at most one new input per simulation tick to preserve discrete input edges.
    while (!p.pendingInputs.empty()) {
        const PlayerInput cmd = p.pendingInputs.front();
        p.pendingInputs.pop_front();
        if (!IsNewerU32(cmd.sequenceNumber, p.lastProcessedInputSequence)) {
            continue;
        }

        const float safeYaw = std::isfinite(cmd.yaw) ? cmd.yaw : 0.0f;
        const float safePitch = std::isfinite(cmd.pitch) ? cmd.pitch : 0.0f;
        const float safeMoveX = std::isfinite(cmd.moveX) ? cmd.moveX : 0.0f;
        const float safeMoveZ = std::isfinite(cmd.moveZ) ? cmd.moveZ : 0.0f;

        p.lastProcessedInputSequence = cmd.sequenceNumber;
        p.activeInputFlags = cmd.inputFlags;
        p.flyMode = p.allowFlyMode && (cmd.flyMode != 0);
        // Keep look values only for replication/debug; they are not used for authoritative movement.
        p.yaw = NormalizeYawDegrees(safeYaw);
        p.pitch = safePitch;
        p.moveX = std::clamp(safeMoveX, -1.0f, 1.0f);
        p.moveZ = std::clamp(safeMoveZ, -1.0f, 1.0f);
        break;
    }

    uint8_t effectiveFlags = p.activeInputFlags;
    float effectiveMoveX = p.moveX;
    float effectiveMoveZ = p.moveZ;
    if (movement.inputSilenceStopSec > 0.0f && movement.inputSilenceStopSec > movement.inputSilenceDecayStartSec) {
        const auto now = Clock::now();
        const float silenceSec = std::chrono::duration<float>(now - p.lastInputReceived).count();
        if (silenceSec > movement.inputSilenceDecayStartSec) {
            const float t = std::clamp(
                (silenceSec - movement.inputSilenceDecayStartSec) /
                (movement.inputSilenceStopSec - movement.inputSilenceDecayStartSec),
                0.0f,
                1.0f
            );
            const float keep = 1.0f - t;
            effectiveMoveX *= keep;
            effectiveMoveZ *= keep;
            if (t >= 1.0f) {
                effectiveFlags &= static_cast<uint8_t>(~kContinuousMoveFlags);
            }
        }
    }

    Shared::Movement::State simState;
    simState.position = p.position;
    simState.velocity = p.velocity;
    simState.onGround = p.onGround;
    simState.flyMode = p.flyMode;
    simState.jumpPressedLastTick = p.jumpPressedLastTick;
    simState.timeSinceGrounded = p.timeSinceGrounded;
    simState.jumpBufferTimer = p.jumpBufferTimer;

    Shared::Movement::InputState simInput;
    simInput.moveX = effectiveMoveX;
    simInput.moveZ = effectiveMoveZ;
    simInput.flags = effectiveFlags;
    simInput.flyMode = p.flyMode;

    Shared::Movement::Options simOptions;
    simOptions.allowFlyMode = p.allowFlyMode;
    simOptions.allowStepUp = true;
    simOptions.requireSprintForStepUp = true;

    Shared::Movement::Simulate(
        simState,
        simInput,
        static_cast<float>(dt),
        movement,
        simOptions,
        [&p, &chunkManager, this](const glm::vec3& testPos) {
            return checkCollision(p, testPos, chunkManager);
        },
        nullptr
    );

    p.position = simState.position;
    p.velocity = simState.velocity;
    p.onGround = simState.onGround;
    p.flyMode = simState.flyMode;
    p.jumpPressedLastTick = simState.jumpPressedLastTick;
    p.timeSinceGrounded = simState.timeSinceGrounded;
    p.jumpBufferTimer = simState.jumpBufferTimer;
}

std::vector<uint8_t> PlayerManager::buildSnapshotFor(PlayerID recipientId, uint32_t serverTick) {
    std::vector<PlayerID> recipients;
    recipients.push_back(recipientId);
    std::vector<std::vector<uint8_t>> snapshots = buildSnapshotsForRecipients(recipients, serverTick);
    if (snapshots.empty()) {
        return {};
    }
    return std::move(snapshots.front());
}

std::vector<std::vector<uint8_t>> PlayerManager::buildSnapshotsForRecipients(
    const std::vector<PlayerID>& recipientIds,
    uint32_t serverTick
) {
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(recipientIds.size());
    if (recipientIds.empty()) {
        return frames;
    }

    std::lock_guard<std::mutex> lock(mtx);
    const auto now = Clock::now();

    std::vector<PlayerSnapshot> players;
    players.reserve(playersById.size());

    for (const auto& kv : playersById) {
        const ServerPlayer& p = kv.second;
        PlayerSnapshot pkt{};
        pkt.id = p.id;
        pkt.px = p.position.x; pkt.py = p.position.y; pkt.pz = p.position.z;
        pkt.vx = p.velocity.x; pkt.vy = p.velocity.y; pkt.vz = p.velocity.z;
        pkt.yaw = p.yaw;
        pkt.pitch = p.pitch;
        pkt.onGround = p.onGround ? 1 : 0;
        pkt.flyMode = p.flyMode ? 1 : 0;
        pkt.allowFlyMode = p.allowFlyMode ? 1 : 0;
        pkt.weaponId = p.equippedWeaponId;
        pkt.health = p.health;
        pkt.isAlive = p.isAlive ? 1 : 0;
        if (p.isAlive || p.respawnAt == Clock::time_point{}) {
            pkt.respawnSeconds = 0.0f;
        }
        else {
            const float remaining = std::chrono::duration<float>(p.respawnAt - now).count();
            pkt.respawnSeconds = std::max(0.0f, remaining);
        }
        players.push_back(pkt);
    }

    const std::vector<uint8_t> playersPayload = SerializePlayerSnapshotsPayload(players);

    for (PlayerID recipientId : recipientIds) {
        auto recipIt = playersById.find(recipientId);
        if (recipIt == playersById.end()) {
            frames.emplace_back();
            continue;
        }

        frames.push_back(BuildSnapshotFrameBytes(
            serverTick,
            recipientId,
            recipIt->second.lastProcessedInputSequence,
            playersPayload
        ));
    }

    return frames;
}

void PlayerManager::sendBytes(const std::shared_ptr<ConnectionHandle>& conn, const std::vector<uint8_t>& buf) {
    if (!conn) return;
    (void)conn;
    (void)buf;
}

void PlayerManager::broadcastSnapshots() {
    std::vector<std::pair<PlayerID, std::shared_ptr<ConnectionHandle>>> recipients;
    {
        std::lock_guard<std::mutex> lock(mtx);
        recipients.reserve(playersById.size());
        for (const auto& kv : playersById) {
            recipients.emplace_back(kv.first, kv.second.conn);
        }
    }

    for (const auto& [id, conn] : recipients) {
        auto buf = buildSnapshotFor(id, 0);
        if (!buf.empty() && conn) {
            sendBytes(conn, buf);
        }
    }
}

std::optional<ServerPlayer> PlayerManager::getPlayerCopy(PlayerID id) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return std::nullopt;
    return it->second;
}

std::vector<ServerPlayer> PlayerManager::getAllPlayersCopy() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<ServerPlayer> players;
    players.reserve(playersById.size());
    for (const auto& kv : playersById) {
        players.push_back(kv.second);
    }
    return players;
}

bool PlayerManager::applyDamage(PlayerID id, float damage, float& outHealthAfter, bool& outKilled) {
    outHealthAfter = 0.0f;
    outKilled = false;
    if (!std::isfinite(damage) || damage <= 0.0f) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    ServerPlayer& target = it->second;
    if (!target.isAlive) {
        outHealthAfter = 0.0f;
        outKilled = false;
        return false;
    }

    target.health = std::max(0.0f, target.health - damage);
    outHealthAfter = target.health;
    if (target.health <= 0.0f) {
        outKilled = true;
        target.health = 0.0f;
        target.isAlive = false;
        target.respawnAt = Clock::now() + respawnDelay;
        target.pendingRespawnRequest = false;
        target.velocity = glm::vec3(0.0f);
        target.flyMode = false;
        target.activeInputFlags = 0;
        target.moveX = 0.0f;
        target.moveZ = 0.0f;
        target.onGround = false;
        target.jumpPressedLastTick = false;
        target.timeSinceGrounded = 0.0f;
        target.jumpBufferTimer = 0.0f;
        target.pendingInputs.clear();
        outHealthAfter = 0.0f;
    }
    return true;
}

bool PlayerManager::requestRespawn(PlayerID id) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) {
        return false;
    }

    ServerPlayer& player = it->second;
    if (player.isAlive) {
        return false;
    }

    const auto now = Clock::now();
    if (player.respawnAt == Clock::time_point{} || now < player.respawnAt) {
        return false;
    }

    player.pendingRespawnRequest = true;
    return true;
}
