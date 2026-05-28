#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class AdminService {
public:
    void Save() const;
    void Load();

    bool SetAdminIdentity(std::string_view identity, bool isAdmin);
    bool IsAdminIdentity(std::string_view identity) const;
    std::vector<std::string> GetAdminIdentities() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_set<std::string> m_adminIdentities;
    static constexpr std::string_view kAdminsFile = "admins.txt";
};

