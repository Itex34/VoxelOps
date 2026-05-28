#pragma once

#include "../persistence/AdminControlService.hpp"
#include "../../game/player/PlayerManager.hpp"

#include <string>
#include <utility>
#include <vector>

class ServerControlService {
public:
    ServerControlService(AdminControlService &adminControlService, PlayerManager &playerManager);

    bool SetAdminByUsername(const std::string &target, bool isAdmin);
    bool IsAdminUsername(const std::string &usernameOrIdentity) const;
    std::vector<std::pair<std::string, bool>> GetConnectedUsers() const;
    std::vector<std::string> GetAdminUsernames() const;

    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled() const;

private:
    AdminControlService &m_adminControlService;
    PlayerManager &m_playerManager;
};
