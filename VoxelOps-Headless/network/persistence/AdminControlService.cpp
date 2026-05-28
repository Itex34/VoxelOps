#include "AdminControlService.hpp"

#include "../core/LockWaitTelemetry.hpp"
#include "../protocol/Validation.hpp"

#include <algorithm>
#include <iostream>

namespace {
bool IsLikelyIdentityToken(const std::string &value) {
    return NetValidation::IsValidIdentity(value);
}
} // namespace

AdminControlService::AdminControlService(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    AdminService &adminService
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_adminService(adminService) {}

bool AdminControlService::SetAdminByUsername(const std::string &target, bool isAdmin) {
    if (target.empty()) {
        return false;
    }

    const std::string trimmedTarget = target;
    std::string requestedIdentity;
    if (trimmedTarget.rfind("id:", 0) == 0) {
        requestedIdentity = NetValidation::NormalizeIdentity(trimmedTarget.substr(3));
    } else {
        requestedIdentity = NetValidation::NormalizeIdentity(trimmedTarget);
    }

    bool changed = false;
    bool persistChanged = false;
    std::string resolvedIdentity;
    std::vector<PlayerID> onlinePlayersToUpdate;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "AdminControlService::SetAdminByUsername"
        );
        for (const auto &[_, session] : m_sessions) {
            if (session.username == trimmedTarget && !session.identity.empty()) {
                resolvedIdentity = session.identity;
                break;
            }
        }
        if (resolvedIdentity.empty() && IsLikelyIdentityToken(requestedIdentity)) {
            resolvedIdentity = requestedIdentity;
        }

        if (resolvedIdentity.empty()) {
            return false;
        }

        persistChanged = m_adminService.SetAdminIdentity(resolvedIdentity, isAdmin);
        changed = persistChanged;

        for (auto &[_, session] : m_sessions) {
            if (session.identity == resolvedIdentity && session.playerId != 0) {
                if (session.isAdmin != isAdmin) {
                    session.isAdmin = isAdmin;
                    changed = true;
                }
                onlinePlayersToUpdate.push_back(session.playerId);
            }
        }
    }

    for (PlayerID playerId : onlinePlayersToUpdate) {
        m_playerManager.setFlyModeAllowed(playerId, isAdmin);
    }
    if (persistChanged) {
        m_adminService.Save();
    }

    std::cout << "[admin] " << (isAdmin ? "granted " : "revoked ") << resolvedIdentity
              << (!onlinePlayersToUpdate.empty() ? " (online)" : " (offline)")
              << (changed ? "" : " [no change]") << "\n";
    return changed;
}

bool AdminControlService::IsAdminUsername(const std::string &usernameOrIdentity) const {
    std::string identity = usernameOrIdentity;
    if (identity.rfind("id:", 0) == 0) {
        identity = identity.substr(3);
    }
    identity = NetValidation::NormalizeIdentity(identity);
    return m_adminService.IsAdminIdentity(identity);
}

std::vector<std::pair<std::string, bool>> AdminControlService::GetConnectedUsers() const {
    std::vector<std::pair<std::string, bool>> users;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_mutex, "AdminControlService::GetConnectedUsers"
        );
        users.reserve(m_sessions.size());
        for (const auto &[_, session] : m_sessions) {
            if (session.username.empty()) {
                continue;
            }
            std::string label = session.username;
            if (!session.identity.empty()) {
                label += " [id:";
                label += session.identity;
                label += "]";
            }
            users.emplace_back(std::move(label), session.isAdmin);
        }
    }
    std::sort(users.begin(), users.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    return users;
}

std::vector<std::string> AdminControlService::GetAdminUsernames() const {
    return m_adminService.GetAdminIdentities();
}
