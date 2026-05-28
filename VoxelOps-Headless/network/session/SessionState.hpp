#pragma once

#include "../core/LockWaitTelemetry.hpp"
#include "ClientSessionManager.hpp"

#include <mutex>
#include <utility>

class SessionState {
public:
    using ClientSession = ClientSessionManager::ClientSession;

    std::mutex &MutexRef();
    ClientSessionManager &SessionsRef();
    const ClientSessionManager &SessionsRef() const;

    void ClearSessions();
    std::vector<std::pair<HSteamNetConnection, ClientSession>> SnapshotSessions();
    size_t CountRegisteredSessions();

    template <typename Func>
    auto WithLock(Func &&func) -> decltype(func(std::declval<ClientSessionManager &>())) {
        auto lk = LockWaitTelemetry::AcquireSessionLock(m_mutex, "SessionState::WithLock");
        return func(m_sessions);
    }

private:
    std::mutex m_mutex;
    ClientSessionManager m_sessions;
};
