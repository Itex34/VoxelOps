#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>
#include <string>
#include <deque>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <string_view>

#include "network/ServerNetwork.hpp"
#include "../Shared/runtime/Paths.hpp"

using namespace std::chrono_literals;

static std::atomic<bool> g_running{ true };
static std::mutex g_consoleQueueMutex;
static std::deque<std::string> g_consoleQueue;

static void handle_signal(int) {
    g_running = false;
}

static std::string trim_copy(const std::string& in) {
    size_t start = 0;
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])) != 0) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
        --end;
    }
    return in.substr(start, end - start);
}

static std::string to_lower_copy(std::string in) {
    std::transform(in.begin(), in.end(), in.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return in;
}

static void print_console_help() {
    std::cout
        << "[Console] Commands:\n"
        << "  help\n"
        << "  players\n"
        << "  admins\n"
        << "  admin grant <username|id:identity>\n"
        << "  admin revoke <username|id:identity>\n"
        << "  debug [on|off]\n"
        << "  stop | quit | exit\n";
}

struct ServerLaunchOptions {
    uint16_t port = 27015;
    bool showHelp = false;
};

static void print_startup_help() {
    std::cout
        << "VoxelOps headless server options:\n"
        << "  --port <port> (default: 27015)\n"
        << "  --help\n";
}

static bool parse_port(std::string_view value, uint16_t& outPort) {
    if (value.empty()) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }

    unsigned long parsed = 0;
    try {
        parsed = std::stoul(std::string(value));
    }
    catch (...) {
        return false;
    }

    if (parsed == 0 || parsed > 65535) {
        return false;
    }
    outPort = static_cast<uint16_t>(parsed);
    return true;
}

static bool parse_launch_options(int argc, char** argv, ServerLaunchOptions& outOptions) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = (argv[i] != nullptr) ? std::string_view(argv[i]) : std::string_view();
        if (arg.empty()) {
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            outOptions.showHelp = true;
            continue;
        }

        if (arg == "--port" || arg == "-p") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::cerr << "Missing value for " << arg << "\n";
                return false;
            }
            uint16_t parsedPort = 0;
            if (!parse_port(argv[++i], parsedPort)) {
                std::cerr << "Invalid port: " << argv[i] << "\n";
                return false;
            }
            outOptions.port = parsedPort;
            continue;
        }

        std::cerr << "Unknown option: " << arg << "\n";
        return false;
    }

    return true;
}

static void process_console_command(const std::string& rawCommand, ServerNetwork& serverNet) {
    const std::string command = trim_copy(rawCommand);
    if (command.empty()) {
        return;
    }

    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;
    cmd = to_lower_copy(cmd);

    if (cmd == "help") {
        print_console_help();
        return;
    }

    if (cmd == "stop" || cmd == "quit" || cmd == "exit") {
        std::cout << "[Console] Stop requested.\n";
        g_running = false;
        serverNet.Stop();
        return;
    }

    if (cmd == "players") {
        const auto users = serverNet.GetConnectedUsers();
        if (users.empty()) {
            std::cout << "[Console] No connected players.\n";
            return;
        }
        std::cout << "[Console] Connected players:\n";
        for (const auto& [name, isAdmin] : users) {
            std::cout << "  - " << name << (isAdmin ? " [admin]" : "") << "\n";
        }
        return;
    }

    if (cmd == "admins") {
        const auto admins = serverNet.GetAdminUsernames();
        if (admins.empty()) {
            std::cout << "[Console] Admin list is empty.\n";
            return;
        }
        std::cout << "[Console] Admin identities:\n";
        for (const auto& name : admins) {
            std::cout << "  - " << name << "\n";
        }
        return;
    }

    if (cmd == "admin") {
        std::string action;
        iss >> action;
        action = to_lower_copy(action);

        if (action == "list") {
            const auto admins = serverNet.GetAdminUsernames();
            if (admins.empty()) {
                std::cout << "[Console] Admin list is empty.\n";
            }
            else {
                std::cout << "[Console] Admin identities:\n";
                for (const auto& name : admins) {
                    std::cout << "  - " << name << "\n";
                }
            }
            return;
        }

        if (action != "grant" && action != "revoke") {
            std::cout << "[Console] Usage: admin grant <username|id:identity> | admin revoke <username|id:identity>\n";
            return;
        }

        std::string username;
        iss >> username;
        username = trim_copy(username);
        if (username.empty()) {
            std::cout << "[Console] Missing target. Usage: admin " << action << " <username|id:identity>\n";
            return;
        }

        const bool isGrant = (action == "grant");
        const bool changed = serverNet.SetAdminByUsername(username, isGrant);
        if (!changed) {
            std::cout << "[Console] No matching user/admin state changed for '" << username << "'.\n";
        }
        return;
    }

    if (cmd == "debug") {
        std::string arg;
        iss >> arg;
        arg = to_lower_copy(arg);

        if (arg.empty()) {
            std::cout << "[Console] debug is " << (serverNet.IsDebugLoggingEnabled() ? "on" : "off") << "\n";
            return;
        }

        if (arg == "on") {
            serverNet.SetDebugLoggingEnabled(true);
            return;
        }
        if (arg == "off") {
            serverNet.SetDebugLoggingEnabled(false);
            return;
        }

        std::cout << "[Console] Usage: debug [on|off]\n";
        return;
    }

    std::cout << "[Console] Unknown command: " << command << "\n";
    print_console_help();
}

int main(int argc, char** argv) {
    Shared::RuntimePaths::Initialize((argc > 0 && argv != nullptr) ? argv[0] : "");

    ServerLaunchOptions launchOptions;
    if (!parse_launch_options(argc, argv, launchOptions)) {
        print_startup_help();
        return 2;
    }
    if (launchOptions.showHelp) {
        print_startup_help();
        return 0;
    }

    std::cout << "VoxelOps headless server starting...\n";
    std::cout << "[Server] Runtime paths: " << Shared::RuntimePaths::Describe() << "\n";

    // Handle Ctrl+C and termination signals
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    const uint16_t port = launchOptions.port;
    ServerNetwork serverNet;

    
    if (!serverNet.Start(port)) {
        std::cerr << "Failed to start ServerNetwork on port " << port << "\n";
        return 1;
    }

    // Launch networking thread
    std::thread netThread([&serverNet]() {
        serverNet.Run(); // blocking loop
        });

    std::thread consoleInputThread([]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lk(g_consoleQueueMutex);
            g_consoleQueue.push_back(line);
        }
    });
    consoleInputThread.detach();

    print_console_help();

    // Simple periodic heartbeat broadcast (every second)
    const auto heartbeatInterval = 1s;
    auto lastHeartbeat = std::chrono::steady_clock::now();

    while (g_running) {
        std::deque<std::string> pendingCommands;
        {
            std::lock_guard<std::mutex> lk(g_consoleQueueMutex);
            pendingCommands.swap(g_consoleQueue);
        }
        for (const std::string& command : pendingCommands) {
            process_console_command(command, serverNet);
        }

        if (!g_running) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= heartbeatInterval) {
            lastHeartbeat = now;

            std::string msg;
            msg.push_back(static_cast<char>(PacketType::Message));
            msg += "server_heartbeat";

            serverNet.BroadcastRaw(msg.data(), static_cast<uint32_t>(msg.size()), k_HSteamNetConnection_Invalid);
            if (serverNet.IsDebugLoggingEnabled()){
                std::cout << "[Server] Heartbeat broadcasted.\n";
            }
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
