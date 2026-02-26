// server_main.cpp — minimal functional VoxelOps server
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>
#include <string>

#include "network/ServerNetwork.hpp"

using namespace std::chrono_literals;

static std::atomic<bool> g_running{ true };

static void handle_signal(int) {
    g_running = false;
}

int main() {
    std::cout << "VoxelOps headless server starting...\n";

    // Handle Ctrl+C and termination signals
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    const uint16_t port = 27015;
    ServerNetwork serverNet;

    
    if (!serverNet.Start(port)) {
        std::cerr << "Failed to start ServerNetwork on port " << port << "\n";
        return 1;
    }

    // Launch networking thread
    std::thread netThread([&serverNet]() {
        serverNet.Run(); // blocking loop
        });

    // Simple periodic heartbeat broadcast (every second)
    const auto heartbeatInterval = 1s;
    auto lastHeartbeat = std::chrono::steady_clock::now();

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= heartbeatInterval) {
            lastHeartbeat = now;

            std::string msg;
            msg.push_back(static_cast<char>(PacketType::Message));
            msg += "server_heartbeat";

            serverNet.BroadcastRaw(msg.data(), static_cast<uint32_t>(msg.size()), k_HSteamNetConnection_Invalid);
            std::cout << "[Server] Heartbeat broadcasted.\n";
        }

        std::this_thread::sleep_for(10ms);
    }

    std::cout << "Shutdown requested. Stopping server...\n";
    serverNet.Stop();
    if (netThread.joinable())
        netThread.join();
    std::cout << "Server stopped\n";
    return 0;
}
