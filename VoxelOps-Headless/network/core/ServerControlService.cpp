#include "ServerControlService.hpp"

#include "DiagnosticsFlags.hpp"

#include <iostream>

ServerControlService::ServerControlService(
    AdminControlService &adminControlService, PlayerManager &playerManager
)
    : m_adminControlService(adminControlService)
    , m_playerManager(playerManager) {}

bool ServerControlService::SetAdminByUsername(const std::string &target, bool isAdmin) {
    return m_adminControlService.SetAdminByUsername(target, isAdmin);
}

bool ServerControlService::IsAdminUsername(const std::string &usernameOrIdentity) const {
    return m_adminControlService.IsAdminUsername(usernameOrIdentity);
}

std::vector<std::pair<std::string, bool>> ServerControlService::GetConnectedUsers() const {
    return m_adminControlService.GetConnectedUsers();
}

std::vector<std::string> ServerControlService::GetAdminUsernames() const {
    return m_adminControlService.GetAdminUsernames();
}

void ServerControlService::SetDebugLoggingEnabled(bool enabled) {
    DiagnosticsFlags::SetAllEnabled(enabled);
    m_playerManager.SetDebugLoggingEnabled(enabled);
    std::cout << "[debug] diagnostics " << (enabled ? "enabled" : "disabled") << "\n";
}

bool ServerControlService::IsDebugLoggingEnabled() const {
    if (DiagnosticsFlags::IsAnyEnabled()) {
        return true;
    }
    return m_playerManager.IsDebugLoggingEnabled();
}
