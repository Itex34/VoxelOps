#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include "../../Shared/network/PacketType.hpp"

class Runtime;

class Network {
public:
    Network();
    ~Network();

    Network(const Network &) = delete;
    Network &operator=(const Network &) = delete;

    bool Start(uint16_t port = 27015);
    void Run();
    void Stop();

    void SaveHistoryToFile();
    void LoadHistoryFromFile();
    void SaveAdminsToFile();
    void LoadAdminsFromFile();

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
    std::unique_ptr<Runtime> m_runtime;
};
