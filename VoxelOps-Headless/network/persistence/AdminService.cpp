#include "AdminService.hpp"

#include "../protocol/Validation.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
bool IsLikelyIdentityToken(const std::string &value) {
    return NetValidation::IsValidIdentity(value);
}
} // namespace

void AdminService::Save() const {
    std::vector<std::string> identities;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        identities.reserve(m_adminIdentities.size());
        for (const auto &identity : m_adminIdentities) {
            if (!identity.empty()) {
                identities.push_back(identity);
            }
        }
    }

    std::sort(identities.begin(), identities.end());
    identities.erase(std::unique(identities.begin(), identities.end()), identities.end());

    std::ofstream fout(std::string(kAdminsFile), std::ios::out | std::ios::trunc);
    if (!fout) {
        return;
    }
    for (const auto &identity : identities) {
        fout << identity << '\n';
    }
}

void AdminService::Load() {
    std::ifstream fin{std::string(kAdminsFile)};
    if (!fin) {
        return;
    }

    std::unordered_set<std::string> loaded;
    std::string line;
    while (std::getline(fin, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start > 0) {
            line.erase(0, start);
        }
        if (line.empty()) {
            continue;
        }
        const std::string identity = NetValidation::NormalizeIdentity(line);
        if (!IsLikelyIdentityToken(identity)) {
            continue;
        }
        loaded.insert(identity);
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_adminIdentities = std::move(loaded);
    }
}

bool AdminService::SetAdminIdentity(std::string_view identity, bool isAdmin) {
    if (identity.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    if (isAdmin) {
        return m_adminIdentities.insert(std::string(identity)).second;
    }
    return m_adminIdentities.erase(std::string(identity)) > 0;
}

bool AdminService::IsAdminIdentity(std::string_view identity) const {
    if (identity.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_adminIdentities.find(std::string(identity)) != m_adminIdentities.end();
}

std::vector<std::string> AdminService::GetAdminIdentities() const {
    std::vector<std::string> identities;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        identities.reserve(m_adminIdentities.size());
        for (const auto &identity : m_adminIdentities) {
            identities.push_back(identity);
        }
    }
    std::sort(identities.begin(), identities.end());
    return identities;
}

