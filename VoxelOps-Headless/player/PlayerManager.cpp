#include "PlayerManager.hpp"
#include "../../Shared/network/PacketType.hpp"
#include "../../Shared/network/Packets.hpp"


#include <cstring>   // memcpy
#include <algorithm>
#include <iostream>
#include <glm/glm.hpp>



PlayerManager::PlayerManager() = default;

PlayerID PlayerManager::addPlayerInternal() {
    return nextId.fetch_add(1, std::memory_order_relaxed);
}

PlayerID PlayerManager::onPlayerConnect(std::shared_ptr<ConnectionHandle> conn, const glm::vec3& spawnPos) {
    std::lock_guard<std::mutex> lock(mtx);
    PlayerID id = addPlayerInternal();
    ServerPlayer p;
    p.id = id;
    p.position = spawnPos;
    p.velocity = glm::vec3(0.0f);
    p.lastHeartbeat = Clock::now();
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

bool PlayerManager::applyAuthoritativeState(PlayerID id, const glm::vec3& position, const glm::vec3& velocity) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return false;

    it->second.position = position;
    it->second.velocity = velocity;
    it->second.onGround = (position.y <= 0.0f);
    it->second.lastHeartbeat = Clock::now();
    return true;
}

void PlayerManager::update(double deltaSeconds) {
    // 1) Simulate physics for each player
    std::vector<PlayerID> toRemove;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& kv : playersById) {
            simulatePhysicsFor(kv.second, deltaSeconds);

            // timeout check
            auto now = Clock::now();
            if (now - kv.second.lastHeartbeat > heartbeatTimeout) {
                toRemove.push_back(kv.first);
            }
        }

        // remove timed-out players
        for (auto id : toRemove) {
            auto it = playersById.find(id);
            if (it != playersById.end()) {
                playersOrder.erase(it->second.orderIt);
                playersById.erase(it);
                std::cout << "Player " << id << " timed out and removed\n";
            }
        }
    }

}

void PlayerManager::simulatePhysicsFor(ServerPlayer& p, double dt) {
    // Very simple integrator; replace with your physics/collision code
    if (!p.onGround) {
        // gravity
        const float g = -9.81f;
        p.velocity.y += g * static_cast<float>(dt);
    }
    // integrate
    p.position += p.velocity * static_cast<float>(dt);

    // naive ground collision
    if (p.position.y <= 0.0f) {
        p.position.y = 0.0f;
        p.velocity.y = 0.0f;
        p.onGround = true;
    }
    else {
        p.onGround = false;
    }
}

std::vector<uint8_t> PlayerManager::buildSnapshotFor(PlayerID recipientId) {
    std::vector<uint8_t> buf;
    std::lock_guard<std::mutex> lock(mtx);

    auto recipIt = playersById.find(recipientId);
    if (recipIt == playersById.end()) return buf;

    // For simplicity: include all players in the snapshot.
    // In production: include only players within recip's interest area (renderDistance / frustum / chunk list).
    size_t count = playersById.size();
    buf.reserve(4 + count * sizeof(PlayerSnapshot));

    // Header: uint32_t count (little-endian)
    uint32_t cnt32 = static_cast<uint32_t>(count);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&cnt32), reinterpret_cast<uint8_t*>(&cnt32) + sizeof(cnt32));

    for (const auto& kv : playersById) {
        const ServerPlayer& p = kv.second;
        PlayerSnapshot pkt;
        pkt.id = p.id;
        pkt.px = p.position.x; pkt.py = p.position.y; pkt.pz = p.position.z;
        pkt.vx = p.velocity.x; pkt.vy = p.velocity.y; pkt.vz = p.velocity.z;
        pkt.yaw = p.yaw;
        pkt.pitch = p.pitch;
        pkt.onGround = p.onGround ? 1 : 0;

        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&pkt);
        buf.insert(buf.end(), raw, raw + sizeof(pkt));
    }
    return buf;
}

void PlayerManager::sendBytes(const std::shared_ptr<ConnectionHandle>& conn, const std::vector<uint8_t>& buf) {
    if (!conn) return;
    // Replace the following with your networking send logic. Example placeholder:
    // ssize_t sent = ::send(conn->socketFd, buf.data(), buf.size(), 0);
    // You should handle partial sends, errors, etc.
    (void)conn;
    (void)buf;
    // For demo: do nothing.
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
        auto buf = buildSnapshotFor(id);
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

void PlayerManager::processClientInput(PlayerID id, const std::vector<uint8_t>& packetData) {
    // Minimal parse of our sample InputPacket (see below). This should be robust to malformed data.
    // For safety, we parse fields manually and then update the server player's input buffer / state.
    if (packetData.size() < 1) return;
    // Example format: [uint8_t type][payload...]
    // We'll assume packetData already represents an Input packet payload for player id.
    std::lock_guard<std::mutex> lock(mtx);
    auto it = playersById.find(id);
    if (it == playersById.end()) return;

    // Very simple: assume packet is of exact size of our sample InputPacket:
    // [uint32_t seq][uint8_t inputFlags][float lookYaw][float lookPitch][float moveX][float moveY]
    struct InputPkt {
        uint32_t seq;
        uint8_t flags;
        float yaw;
        float pitch;
        float moveX;
        float moveY;
    };
    if (packetData.size() < sizeof(InputPkt)) return;
    InputPkt pkt;
    memcpy(&pkt, packetData.data(), sizeof(InputPkt));

    // Apply / queue the input. Here we apply directly for simplicity:
    ServerPlayer& p = it->second;
    p.yaw = pkt.yaw;
    p.pitch = pkt.pitch;

    // Interpret flags: bit 0 = forward, 1 = back, 2 = left, 3 = right, 4 = jump
    glm::vec3 move(0.0f);
    float speed = 5.0f;
    if (pkt.flags & 0x01) move.z += 1.0f;
    if (pkt.flags & 0x02) move.z -= 1.0f;
    if (pkt.flags & 0x04) move.x -= 1.0f;
    if (pkt.flags & 0x08) move.x += 1.0f;
    // local move vector normalized
    if (glm::length(move) > 0.001f) {
        move = glm::normalize(move) * speed;
    }
    // rotate by yaw (assuming yaw in degrees)
    float radYaw = glm::radians(p.yaw);
    glm::vec3 forward = glm::vec3(cos(radYaw), 0.0f, sin(radYaw));
    glm::vec3 right = glm::vec3(-forward.z, 0.0f, forward.x);
    p.velocity.x = forward.x * move.z + right.x * move.x;
    p.velocity.z = forward.z * move.z + right.z * move.x;

    if ((pkt.flags & 0x10) && p.onGround) {
        p.velocity.y = 8.5f; // jump impulse
        p.onGround = false;
    }
}
