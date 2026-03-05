#include "ServerNetwork.hpp"

#include <cctype>

namespace {
bool IsValidIdentityChar(char c)
{
    return
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '_' ||
        c == '-';
}

std::string NormalizeIdentity(std::string value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (IsValidIdentityChar(lower)) {
            out.push_back(lower);
        }
    }
    if (out.size() > kMaxConnectIdentityChars) {
        out.resize(kMaxConnectIdentityChars);
    }
    return out;
}

bool IsLikelyIdentityToken(const std::string& value)
{
    if (value.empty() || value.size() > kMaxConnectIdentityChars) {
        return false;
    }
    for (char c : value) {
        if (!IsValidIdentityChar(c)) {
            return false;
        }
    }
    return true;
}
}

void ServerNetwork::SaveHistoryToFile()
{
    std::ofstream fout(HISTORY_FILE, std::ios::out | std::ios::trunc);
    if (!fout) return;
    for (auto& m : m_messageHistory) {
        std::string msg = m.second;
        std::replace(msg.begin(), msg.end(), '\n', ' ');
        fout << m.first << ':' << msg << '\n';
    }
}

void ServerNetwork::LoadHistoryFromFile() {
    std::ifstream fin(HISTORY_FILE);
    if (!fin) return;
    m_messageHistory.clear();
    std::string line;
    while (std::getline(fin, line)) {
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string user = line.substr(0, pos);
        std::string msg = line.substr(pos + 1);
        m_messageHistory.emplace_back(user, msg);
    }
}

void ServerNetwork::SaveAdminsToFile()
{
    std::vector<std::string> identities;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        identities.reserve(m_adminIdentities.size());
        for (const auto& identity : m_adminIdentities) {
            if (!identity.empty()) {
                identities.push_back(identity);
            }
        }
    }

    std::sort(identities.begin(), identities.end());
    identities.erase(std::unique(identities.begin(), identities.end()), identities.end());

    std::ofstream fout(ADMINS_FILE, std::ios::out | std::ios::trunc);
    if (!fout) return;
    for (const auto& identity : identities) {
        fout << identity << '\n';
    }
}

void ServerNetwork::LoadAdminsFromFile()
{
    std::ifstream fin(ADMINS_FILE);
    if (!fin) return;

    std::unordered_set<std::string> loaded;
    std::string line;
    while (std::getline(fin, line)) {
        // trim simple ASCII whitespace at both ends
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
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
        const std::string identity = NormalizeIdentity(line);
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

bool ServerNetwork::SetAdminByUsername(const std::string& target, bool isAdmin)
{
    if (target.empty()) {
        return false;
    }

    const std::string trimmedTarget = target;
    std::string requestedIdentity;
    if (trimmedTarget.rfind("id:", 0) == 0) {
        requestedIdentity = NormalizeIdentity(trimmedTarget.substr(3));
    }
    else {
        requestedIdentity = NormalizeIdentity(trimmedTarget);
    }

    bool changed = false;
    bool persistChanged = false;
    std::string resolvedIdentity;
    std::vector<PlayerID> onlinePlayersToUpdate;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto& [_, session] : m_clients) {
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
        }
        else {
            persistChanged = (m_adminIdentities.erase(resolvedIdentity) > 0);
        }
        changed = persistChanged;

        for (auto& [_, session] : m_clients) {
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

    std::cout
        << "[admin] " << (isAdmin ? "granted " : "revoked ")
        << resolvedIdentity
        << (!onlinePlayersToUpdate.empty() ? " (online)" : " (offline)")
        << (changed ? "" : " [no change]") << "\n";
    return changed;
}

bool ServerNetwork::IsAdminUsername(const std::string& usernameOrIdentity)
{
    std::string identity = usernameOrIdentity;
    if (identity.rfind("id:", 0) == 0) {
        identity = identity.substr(3);
    }
    identity = NormalizeIdentity(identity);
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_adminIdentities.find(identity) != m_adminIdentities.end();
}

std::vector<std::pair<std::string, bool>> ServerNetwork::GetConnectedUsers()
{
    std::vector<std::pair<std::string, bool>> users;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        users.reserve(m_clients.size());
        for (const auto& [_, session] : m_clients) {
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
    std::sort(users.begin(), users.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return users;
}

std::vector<std::string> ServerNetwork::GetAdminUsernames()
{
    std::vector<std::string> identities;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        identities.reserve(m_adminIdentities.size());
        for (const auto& identity : m_adminIdentities) {
            identities.push_back(identity);
        }
    }
    std::sort(identities.begin(), identities.end());
    return identities;
}

