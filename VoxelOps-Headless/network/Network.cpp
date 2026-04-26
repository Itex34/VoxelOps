#include "ServerNetwork.hpp"

#include "core/ServerRuntime.hpp"

ServerNetwork::ServerNetwork() : m_runtime(std::make_unique<ServerRuntime>()) {}

ServerNetwork::~ServerNetwork() = default;

bool ServerNetwork::Start(uint16_t port) {
    return m_runtime->Start(port);
}

void ServerNetwork::Run() {
    m_runtime->Run();
}

void ServerNetwork::Stop() {
    m_runtime->Stop();
}

void ServerNetwork::SaveHistoryToFile() {
    m_runtime->SaveHistoryToFile();
}

void ServerNetwork::LoadHistoryFromFile() {
    m_runtime->LoadHistoryFromFile();
}

void ServerNetwork::SaveAdminsToFile() {
    m_runtime->SaveAdminsToFile();
}

void ServerNetwork::LoadAdminsFromFile() {
    m_runtime->LoadAdminsFromFile();
}

bool ServerNetwork::SetAdminByUsername(const std::string &username, bool isAdmin) {
    return m_runtime->SetAdminByUsername(username, isAdmin);
}

bool ServerNetwork::IsAdminUsername(const std::string &username) {
    return m_runtime->IsAdminUsername(username);
}

std::vector<std::pair<std::string, bool>> ServerNetwork::GetConnectedUsers() {
    return m_runtime->GetConnectedUsers();
}

std::vector<std::string> ServerNetwork::GetAdminUsernames() {
    return m_runtime->GetAdminUsernames();
}

void ServerNetwork::SetDebugLoggingEnabled(bool enabled) {
    m_runtime->SetDebugLoggingEnabled(enabled);
}

bool ServerNetwork::IsDebugLoggingEnabled() {
    return m_runtime->IsDebugLoggingEnabled();
}

void ServerNetwork::BroadcastRaw(const void *data, uint32_t len, HSteamNetConnection except) {
    m_runtime->BroadcastRaw(data, len, except);
}
