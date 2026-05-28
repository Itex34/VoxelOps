#include "ClientSessionManager.hpp"

size_t ClientSessionManager::erase(HSteamNetConnection conn) {
    ReleasePendingRegistration(conn);
    for (auto it = m_connectionByPlayerId.begin(); it != m_connectionByPlayerId.end();) {
        if (it->second == conn) {
            it = m_connectionByPlayerId.erase(it);
        } else {
            ++it;
        }
    }
    return m_clients.erase(conn);
}

void ClientSessionManager::clear() {
    m_clients.clear();
    ClearPendingRegistrationState();
    ClearPlayerBindings();
}

std::pair<ClientSessionManager::iterator, bool>
ClientSessionManager::AddConnection(HSteamNetConnection conn) {
    return m_clients.emplace(conn, ClientSession{});
}

bool ClientSessionManager::RemoveConnection(
    HSteamNetConnection conn, ClientSession *removedSession
) {
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) {
        ReleasePendingRegistration(conn);
        return false;
    }
    if (removedSession != nullptr) {
        *removedSession = it->second;
    }
    (void)erase(conn);
    return true;
}

ClientSessionManager::ClientSession *ClientSessionManager::FindSession(HSteamNetConnection conn) {
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) {
        return nullptr;
    }
    return &it->second;
}

const ClientSessionManager::ClientSession *
ClientSessionManager::FindSession(HSteamNetConnection conn) const {
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<std::pair<HSteamNetConnection, ClientSessionManager::ClientSession>>
ClientSessionManager::SnapshotSessions() const {
    std::vector<std::pair<HSteamNetConnection, ClientSession>> snapshot;
    snapshot.reserve(m_clients.size());
    for (const auto &kv : m_clients) {
        snapshot.push_back(kv);
    }
    return snapshot;
}

size_t ClientSessionManager::CountRegisteredSessions() const {
    size_t count = 0;
    for (const auto &[_, session] : m_clients) {
        if (session.playerId != 0 && !session.username.empty()) {
            ++count;
        }
    }
    return count;
}

void ClientSessionManager::ReleasePendingRegistration(HSteamNetConnection conn) {
    auto pendingIdentityIt = m_pendingIdentityByConnection.find(conn);
    if (pendingIdentityIt != m_pendingIdentityByConnection.end()) {
        if (!pendingIdentityIt->second.empty()) {
            m_pendingIdentities.erase(pendingIdentityIt->second);
        }
        m_pendingIdentityByConnection.erase(pendingIdentityIt);
    }
    m_pendingUsernameByConnection.erase(conn);
}

void ClientSessionManager::ClearPendingRegistrationState() {
    m_pendingIdentityByConnection.clear();
    m_pendingUsernameByConnection.clear();
    m_pendingIdentities.clear();
}

void ClientSessionManager::MarkPendingRegistration(
    HSteamNetConnection conn, std::string identity, std::string username
) {
    ReleasePendingRegistration(conn);
    if (!identity.empty()) {
        m_pendingIdentities.insert(identity);
    }
    m_pendingIdentityByConnection[conn] = std::move(identity);
    m_pendingUsernameByConnection[conn] = std::move(username);
}

bool ClientSessionManager::IsIdentityPending(std::string_view identity) const {
    if (identity.empty()) {
        return false;
    }
    return m_pendingIdentities.find(std::string(identity)) != m_pendingIdentities.end();
}

bool ClientSessionManager::IsUsernamePendingForOtherConnection(
    HSteamNetConnection exceptConn, std::string_view username
) const {
    if (username.empty()) {
        return false;
    }
    for (const auto &[conn, pendingUsername] : m_pendingUsernameByConnection) {
        if (conn == exceptConn || pendingUsername.empty()) {
            continue;
        }
        if (pendingUsername == username) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<HSteamNetConnection, std::string>>
ClientSessionManager::SnapshotPendingUsernames() const {
    std::vector<std::pair<HSteamNetConnection, std::string>> snapshot;
    snapshot.reserve(m_pendingUsernameByConnection.size());
    for (const auto &kv : m_pendingUsernameByConnection) {
        snapshot.push_back(kv);
    }
    return snapshot;
}

void ClientSessionManager::BindPlayerConnection(PlayerID playerId, HSteamNetConnection conn) {
    if (playerId == 0) {
        return;
    }
    m_connectionByPlayerId[playerId] = conn;
}

void ClientSessionManager::UnbindPlayer(PlayerID playerId) {
    if (playerId == 0) {
        return;
    }
    m_connectionByPlayerId.erase(playerId);
}

void ClientSessionManager::ClearPlayerBindings() {
    m_connectionByPlayerId.clear();
}

std::optional<HSteamNetConnection>
ClientSessionManager::FindConnectionByPlayerId(PlayerID playerId) const {
    auto it = m_connectionByPlayerId.find(playerId);
    if (it == m_connectionByPlayerId.end()) {
        return std::nullopt;
    }
    return it->second;
}
