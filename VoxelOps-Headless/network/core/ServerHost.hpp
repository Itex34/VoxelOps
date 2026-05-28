#pragma once

#include "NetworkRuntime.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class ServerComposition;

class ServerHost {
public:
    ServerHost();
    ~ServerHost();

    ServerHost(const ServerHost &) = delete;
    ServerHost &operator=(const ServerHost &) = delete;

    bool Start(uint16_t port = 27015);
    void Run();
    void Stop();

    bool SetAdminByUsername(const std::string &username, bool isAdmin);
    bool IsAdminUsername(const std::string &username);
    std::vector<std::pair<std::string, bool>> GetConnectedUsers();
    std::vector<std::string> GetAdminUsernames();
    void SetDebugLoggingEnabled(bool enabled);
    bool IsDebugLoggingEnabled();

    void BroadcastRaw(
        const void *data, uint32_t len, HSteamNetConnection except = k_HSteamNetConnection_Invalid
    );

private:
    void ShutdownNetworking();
    void ResetRuntimeState();
    bool StartNetworking(uint16_t port);
    void StartBackgroundServices();
    void ShutdownClientSessions();
    void SaveHistoryToFile();
    void LoadHistoryFromFile();
    void SaveAdminsToFile();
    void LoadAdminsFromFile();

    static void
    SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *pInfo);
    static std::atomic<ServerHost *> s_instance;

private:
    std::atomic<bool> m_quit;
    std::atomic<bool> m_started{false};
    std::mutex m_shutdownMutex;
    bool m_shutdownComplete = false;

    NetworkRuntime m_networkRuntime;
    std::unique_ptr<ServerComposition> m_composition;
};
