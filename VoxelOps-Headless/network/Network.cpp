#include "Network.hpp"

#include "core/Runtime.hpp"

Network::Network() : m_runtime(std::make_unique<Runtime>()) {}

Network::~Network() = default;

bool Network::Start(uint16_t port) {
    return m_runtime->Start(port);
}

void Network::Run() {
    m_runtime->Run();
}

void Network::Stop() {
    m_runtime->Stop();
}

void Network::SaveHistoryToFile() {
    m_runtime->SaveHistoryToFile();
}

void Network::LoadHistoryFromFile() {
    m_runtime->LoadHistoryFromFile();
}

void Network::SaveAdminsToFile() {
    m_runtime->SaveAdminsToFile();
}

void Network::LoadAdminsFromFile() {
    m_runtime->LoadAdminsFromFile();
}

bool Network::SetAdminByUsername(const std::string &username, bool isAdmin) {
    return m_runtime->SetAdminByUsername(username, isAdmin);
}

bool Network::IsAdminUsername(const std::string &username) {
    return m_runtime->IsAdminUsername(username);
}

std::vector<std::pair<std::string, bool>> Network::GetConnectedUsers() {
    return m_runtime->GetConnectedUsers();
}

std::vector<std::string> Network::GetAdminUsernames() {
    return m_runtime->GetAdminUsernames();
}

void Network::SetDebugLoggingEnabled(bool enabled) {
    m_runtime->SetDebugLoggingEnabled(enabled);
}

bool Network::IsDebugLoggingEnabled() {
    return m_runtime->IsDebugLoggingEnabled();
}

void Network::BroadcastRaw(const void *data, uint32_t len, HSteamNetConnection except) {
    m_runtime->BroadcastRaw(data, len, except);
}
