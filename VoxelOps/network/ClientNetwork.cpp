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

void ClientNetwork::SetConnectionStatus(
    ConnectionState state, std::string text, bool allowReconnect
) {
    m_connectionState = state;
    m_connectionStatus = std::move(text);
    m_allowAutoReconnect = allowReconnect;
} 