#include "ChatService.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

void ChatService::AddMessage(std::string username, std::string message) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_messageHistory.emplace_back(std::move(username), std::move(message));
}

void ChatService::Save() const {
    std::vector<std::pair<std::string, std::string>> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        snapshot = m_messageHistory;
    }

    std::ofstream fout(std::string(kHistoryFile), std::ios::out | std::ios::trunc);
    if (!fout) {
        return;
    }
    for (const auto &entry : snapshot) {
        std::string msg = entry.second;
        std::replace(msg.begin(), msg.end(), '\n', ' ');
        fout << entry.first << ':' << msg << '\n';
    }
}

void ChatService::Load() {
    std::ifstream fin{std::string(kHistoryFile)};
    if (!fin) {
        return;
    }

    std::vector<std::pair<std::string, std::string>> loaded;
    std::string line;
   
    while (std::getline(fin, line)) {
        const size_t pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        loaded.emplace_back(line.substr(0, pos), line.substr(pos + 1));
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_messageHistory = std::move(loaded);
    }
}

std::vector<std::pair<std::string, std::string>> ChatService::GetHistory() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_messageHistory;
}
