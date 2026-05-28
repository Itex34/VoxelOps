#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <mutex>

class ChatService {
public:
    void AddMessage(std::string username, std::string message);
    void Save() const;
    void Load();

    std::vector<std::pair<std::string, std::string>> GetHistory() const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::pair<std::string, std::string>> m_messageHistory;
    static constexpr std::string_view kHistoryFile = "chat_history.txt";
};
