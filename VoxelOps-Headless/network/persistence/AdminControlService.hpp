#pragma once

#include "../../../Shared/player/PlayerID.hpp"
#include "../session/ClientSessionManager.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "AdminService.hpp"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

class AdminControlService {
public:
    AdminControlService(
        std::mutex &mutex,
        ClientSessionManager &sessions,
        PlayerManager &playerManager,
        AdminService &adminService
    );

    bool SetAdminByUsername(const std::string &target, bool isAdmin);
    bool IsAdminUsername(const std::string &usernameOrIdentity) const;
    std::vector<std::pair<std::string, bool>> GetConnectedUsers() const;
    std::vector<std::string> GetAdminUsernames() const;

private:
    std::mutex &m_mutex;
    ClientSessionManager &m_sessions;
    PlayerManager &m_playerManager;
    AdminService &m_adminService;
};
