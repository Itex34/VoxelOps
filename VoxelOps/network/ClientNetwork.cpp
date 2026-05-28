#include "ClientNetwork.hpp"

#include <utility>

ClientNetwork::ClientNetwork() = default;

ClientNetwork::~ClientNetwork() {
    Shutdown();
}

ClientNetwork::ConnectionState ClientNetwork::GetConnectionState() const noexcept {
    return m_connectionState;
}

const std::string &ClientNetwork::GetConnectionStatusText() const noexcept {
    return m_connectionStatus;
}

const std::string &ClientNetwork::GetAssignedUsername() const noexcept {
    return m_assignedUsername;
}

bool ClientNetwork::ShouldAutoReconnect() const noexcept {
    return m_allowAutoReconnect;
}

bool ClientNetwork::ShouldSendChunkResyncForOverflow(const glm::ivec3 &chunkPos) {
    constexpr auto kOverflowResyncCooldown = std::chrono::milliseconds(400);
    const auto now = std::chrono::steady_clock::now();
    const ChunkCoordKey key{chunkPos.x, chunkPos.y, chunkPos.z};
    auto it = m_chunkResyncOverflowCooldownUntil.find(key);
    if (it != m_chunkResyncOverflowCooldownUntil.end() && now < it->second) {
        return false;
    }
    m_chunkResyncOverflowCooldownUntil[key] = now + kOverflowResyncCooldown;
    return true;
}

void ClientNetwork::PruneChunkResyncOverflowState() {
    constexpr auto kOverflowResyncPruneSlack = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_chunkResyncOverflowCooldownUntil.begin();
         it != m_chunkResyncOverflowCooldownUntil.end();) {
        if (now >= (it->second + kOverflowResyncPruneSlack)) {
            it = m_chunkResyncOverflowCooldownUntil.erase(it);
        } else {
            ++it;
        }
    }
}

void ClientNetwork::SetConnectionStatus(
    ConnectionState state, std::string text, bool allowReconnect
) {
    m_connectionState = state;
    m_connectionStatus = std::move(text);
    m_allowAutoReconnect = allowReconnect;
} 
