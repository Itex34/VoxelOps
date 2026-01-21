#pragma once


#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>


#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>


#include "../../Shared/network/PacketType.hpp"   // for packet types
#include "../../Shared/network/Packets.hpp"   


class ServerNetwork {
public:



    ServerNetwork();
    ~ServerNetwork();

    // non-copyable
    ServerNetwork(const ServerNetwork&) = delete;
    ServerNetwork& operator=(const ServerNetwork&) = delete;

    // Initialize the networking system and start listening on the given port.
    // Returns true on success.
    bool Start(uint16_t port = 27015);

    // Run the main server loop. Returns when stopped (e.g., via Stop() or SIGINT).
    void Run();

    // Signal server to stop. Run() will return shortly thereafter.
    void Stop();

    void SaveHistoryToFile();
    void LoadHistoryFromFile();



    // static pointer to the currently running instance for the callback bridge
    static ServerNetwork* s_instance;

    void BroadcastRaw(const void* data, uint32_t len, HSteamNetConnection except = k_HSteamNetConnection_Invalid);

private:
    // Internal helpers
    void MainLoop();
    static std::string ReadStringFromPacket(const void* data, uint32_t size, size_t offset = 1);

    // Callback bridge: Steam expects a free function pointer; we implement a static
    // bridge function that calls the instance method.
    static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

private:
    std::atomic<bool> m_quit;
    std::mutex m_mutex;

    // connection -> username
    std::unordered_map<HSteamNetConnection, std::string> m_clients;

    // (username, message)
    std::vector<std::pair<std::string, std::string>> m_messageHistory;
    const char* HISTORY_FILE = "chat_history.txt";

    HSteamNetPollGroup m_pollGroup;
    HSteamListenSocket m_listenSock;

};
