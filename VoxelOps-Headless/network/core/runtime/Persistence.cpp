#include "../Runtime.hpp"
#include "../../protocol/Validation.hpp"

namespace {
    bool IsLikelyIdentityToken(const std::string &value) {
        return NetValidation::IsValidIdentity(value);
    }
} // namespace

void Runtime::SaveHistoryToFile() {
    std::ofstream fout(HISTORY_FILE, std::ios::out | std::ios::trunc);
    if (!fout)
        return;
    for (auto &m : m_messageHistory) {
        std::string msg = m.second;
        std::replace(msg.begin(), msg.end(), '\n', ' ');
        fout << m.first << ':' << msg << '\n';
    }
}

void Runtime::LoadHistoryFromFile() {
    std::ifstream fin(HISTORY_FILE);
    if (!fin)
        return;
    m_messageHistory.clear();
    std::string line;
    while (std::getline(fin, line)) {
        auto pos = line.find(':');
        if (pos == std::string::npos)
            continue;
        std::string user = line.substr(0, pos);
        std::string msg = line.substr(pos + 1);
        m_messageHistory.emplace_back(user, msg);
    }
}

void Runtime::SaveAdminsToFile() {
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

    std::ofstream fout(ADMINS_FILE, std::ios::out | std::ios::trunc);
    if (!fout)
        return;
    for (const auto &identity : identities) {
        fout << identity << '\n';
    }
}

void Runtime::LoadAdminsFromFile() {
    std::ifstream fin(ADMINS_FILE);
    if (!fin)
        return;

    std::unordered_set<std::string> loaded;
    std::string line;
    while (std::getline(fin, line)) {
        // trim simple ASCII whitespace at both ends
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

bool Runtime::SetAdminByUsername(const std::string &target, bool isAdmin) {
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
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto &[_, session] : m_clients) {
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

        if (isAdmin) {
            persistChanged = m_adminIdentities.insert(resolvedIdentity).second;
        } else {
            persistChanged = (m_adminIdentities.erase(resolvedIdentity) > 0);
        }
        changed = persistChanged;

        for (auto &[_, session] : m_clients) {
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
        SaveAdminsToFile();
    }

    std::cout << "[admin] " << (isAdmin ? "granted " : "revoked ") << resolvedIdentity
              << (!onlinePlayersToUpdate.empty() ? " (online)" : " (offline)")
              << (changed ? "" : " [no change]") << "\n";
    return changed;
}

bool Runtime::IsAdminUsername(const std::string &usernameOrIdentity) {
    std::string identity = usernameOrIdentity;
    if (identity.rfind("id:", 0) == 0) {
        identity = identity.substr(3);
    }
    identity = NetValidation::NormalizeIdentity(identity);
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_adminIdentities.find(identity) != m_adminIdentities.end();
}

std::vector<std::pair<std::string, bool>> Runtime::GetConnectedUsers() {
    std::vector<std::pair<std::string, bool>> users;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        users.reserve(m_clients.size());
        for (const auto &[_, session] : m_clients) {
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

std::vector<std::string> Runtime::GetAdminUsernames() {
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
