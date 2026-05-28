#include "SessionState.hpp"

std::mutex &SessionState::MutexRef() {
    return m_mutex;
}

ClientSessionManager &SessionState::SessionsRef() {
    return m_sessions;
}

const ClientSessionManager &SessionState::SessionsRef() const {
    return m_sessions;
}

void SessionState::ClearSessions() {
    auto lk = LockWaitTelemetry::AcquireSessionLock(m_mutex, "SessionState::ClearSessions");
    m_sessions.clear();
}

std::vector<std::pair<HSteamNetConnection, SessionState::ClientSession>> SessionState::SnapshotSessions() {
    auto lk = LockWaitTelemetry::AcquireSessionLock(m_mutex, "SessionState::SnapshotSessions");
    return m_sessions.SnapshotSessions();
}

size_t SessionState::CountRegisteredSessions() {
    auto lk = LockWaitTelemetry::AcquireSessionLock(m_mutex, "SessionState::CountRegisteredSessions");
    return m_sessions.CountRegisteredSessions();
}
