#include "ServerHost.hpp"

#include "ServerComposition.hpp"

#include <iostream>
#include <stdexcept>

std::atomic<ServerHost *> ServerHost::s_instance{nullptr};

ServerHost::ServerHost()
    : m_quit(false)
    , m_composition(std::make_unique<ServerComposition>(m_quit, m_networkRuntime.PollGroupRef())) {
    ServerHost *expected = nullptr;
    if (!s_instance.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        throw std::runtime_error("Only one ServerHost instance is allowed");
    }
}

ServerHost::~ServerHost() {
    Stop();
    ShutdownNetworking();

    ServerHost *expected = this;
    (void)s_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

bool ServerHost::Start(uint16_t port) {
    if (m_started.load(std::memory_order_acquire)) {
        std::cerr << "Network already started\n";
        return false;
    }

    ResetRuntimeState();
    LoadHistoryFromFile();
    LoadAdminsFromFile();
    if (!StartNetworking(port)) {
        return false;
    }
    StartBackgroundServices();
    m_started.store(true, std::memory_order_release);
    return true;
}

void ServerHost::Run() {
    if (!m_started.load(std::memory_order_acquire)) {
        std::cerr << "ServerHost::Run called before Start\n";
        return;
    }
    m_composition->RunTickLoop();
    ShutdownNetworking();
}

void ServerHost::Stop() {
    m_quit.store(true, std::memory_order_release);
}

bool ServerHost::SetAdminByUsername(const std::string &target, bool isAdmin) {
    return m_composition->SetAdminByUsername(target, isAdmin);
}

bool ServerHost::IsAdminUsername(const std::string &usernameOrIdentity) {
    return m_composition->IsAdminUsername(usernameOrIdentity);
}

std::vector<std::pair<std::string, bool>> ServerHost::GetConnectedUsers() {
    return m_composition->GetConnectedUsers();
}

std::vector<std::string> ServerHost::GetAdminUsernames() {
    return m_composition->GetAdminUsernames();
}

void ServerHost::SetDebugLoggingEnabled(bool enabled) {
    m_composition->SetDebugLoggingEnabled(enabled);
}

bool ServerHost::IsDebugLoggingEnabled() {
    return m_composition->IsDebugLoggingEnabled();
}

void ServerHost::BroadcastRaw(const void *data, uint32_t len, HSteamNetConnection except) {
    m_composition->BroadcastRaw(data, len, except);
}

void ServerHost::ShutdownNetworking() {
    bool shouldShutdown = false;
    {
        std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
        if (!m_shutdownComplete) {
            m_shutdownComplete = true;
            shouldShutdown = true;
        }
    }
    if (!shouldShutdown) {
        return;
    }

    m_composition->StopBackgroundServices();
    SaveHistoryToFile();
    SaveAdminsToFile();
    ShutdownClientSessions();

    (void)m_started.exchange(false, std::memory_order_acq_rel);
    m_networkRuntime.Shutdown();
}

void ServerHost::ResetRuntimeState() {
    m_quit.store(false, std::memory_order_release);
    m_composition->ResetRuntimeState();
    {
        std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
        m_shutdownComplete = false;
    }
}

bool ServerHost::StartNetworking(uint16_t port) {
    bool boundAddressAvailable = false;
    SteamNetworkingIPAddr boundAddr;
    if (!m_networkRuntime.Start(
            port,
            &ServerHost::SteamNetConnectionStatusChangedCallback,
            boundAddressAvailable,
            boundAddr
        )) {
        return false;
    }

    if (boundAddressAvailable) {
        const uint32_t ipv4 = boundAddr.GetIPv4();
        if (ipv4 != 0) {
            std::cout << "Server listening on "
                      << ((ipv4 >> 24) & 0xff) << "."
                      << ((ipv4 >> 16) & 0xff) << "."
                      << ((ipv4 >> 8) & 0xff) << "."
                      << (ipv4 & 0xff) << ":"
                      << boundAddr.m_port
                      << " (Ctrl+C to quit)\n";
        } else {
            std::cout << "Server listening on UDP port " << boundAddr.m_port
                      << " (Ctrl+C to quit)\n";
        }
    } else {
        std::cout << "Server listening on UDP port " << port << " (Ctrl+C to quit)\n";
    }
    return true;
}

void ServerHost::StartBackgroundServices() {
    m_composition->StartBackgroundServices();
}

void ServerHost::ShutdownClientSessions() {
    m_composition->ShutdownClientSessions();
}

void ServerHost::SaveHistoryToFile() {
    m_composition->SaveHistoryToFile();
}

void ServerHost::LoadHistoryFromFile() {
    m_composition->LoadHistoryFromFile();
}

void ServerHost::SaveAdminsToFile() {
    m_composition->SaveAdminsToFile();
}

void ServerHost::LoadAdminsFromFile() {
    m_composition->LoadAdminsFromFile();
}

void ServerHost::SteamNetConnectionStatusChangedCallback(
    SteamNetConnectionStatusChangedCallback_t *pInfo
) {
    ServerHost *instance = s_instance.load(std::memory_order_acquire);
    if (instance != nullptr && instance->m_composition != nullptr) {
        instance->m_composition->OnConnectionStatusChanged(pInfo);
    }
}
