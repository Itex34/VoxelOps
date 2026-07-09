#include "ClientNetwork.hpp"

#include "../../Shared/network/IdentityValidation.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ws2tcpip.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace {

    constexpr const char *kClientIdentityFileName = "client_identity.txt";

    std::string NormalizeIdentity(std::string identity) {
        return Shared::NetValidation::NormalizeIdentity(identity);
    }

    bool IsValidIdentity(const std::string &identity) {
        return Shared::NetValidation::IsValidIdentity(identity);
    }

    std::filesystem::path ResolveIdentityFilePath() {
        const char *localAppData = std::getenv("LOCALAPPDATA");
        if (localAppData != nullptr && localAppData[0] != '\0') {
            return std::filesystem::path(localAppData) / "VoxelOps" / kClientIdentityFileName;
        }
        return std::filesystem::current_path() / kClientIdentityFileName;
    }

    std::string GenerateIdentityToken() {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<uint32_t> dist(0u, 0xFFFFFFFFu);
        std::ostringstream out;
        out << "id-";
        for (int i = 0; i < 5; ++i) {
            const uint32_t part = dist(rng);
            out.width(8);
            out.fill('0');
            out << std::hex << std::nouppercase << part;
        }
        std::string token = out.str();
        if (token.size() > kMaxConnectIdentityChars) {
            token.resize(kMaxConnectIdentityChars);
        }
        return token;
    }

    bool
    ResolveHostToAddress(std::string_view host, uint16_t port, SteamNetworkingIPAddr &outAddr) {
        if (host.empty()) {
            return false;
        }

        std::string hostStr(host);

        SteamNetworkingIPAddr parsedAddr;
        parsedAddr.Clear();
        if (parsedAddr.ParseString(hostStr.c_str())) {
            parsedAddr.m_port = port;
            outAddr = parsedAddr;
            return true;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo *results = nullptr;
        const int gaiError = getaddrinfo(hostStr.c_str(), nullptr, &hints, &results);
        if (gaiError != 0 || results == nullptr) {
            if (results != nullptr) {
                freeaddrinfo(results);
            }
            return false;
        }

        bool found = false;
        char addressBuffer[INET6_ADDRSTRLEN] = {};
        for (addrinfo *it = results; it != nullptr; it = it->ai_next) {
            const void *rawAddress = nullptr;
            if (it->ai_family == AF_INET) {
                const sockaddr_in *addr4 = reinterpret_cast<const sockaddr_in *>(it->ai_addr);
                rawAddress = &addr4->sin_addr;
            } else if (it->ai_family == AF_INET6) {
                const sockaddr_in6 *addr6 = reinterpret_cast<const sockaddr_in6 *>(it->ai_addr);
                rawAddress = &addr6->sin6_addr;
            } else {
                continue;
            }

            if (inet_ntop(it->ai_family, rawAddress, addressBuffer, sizeof(addressBuffer)) ==
                nullptr) {
                continue;
            }

            SteamNetworkingIPAddr addr;
            addr.Clear();
            if (!addr.ParseString(addressBuffer)) {
                continue;
            }

            addr.m_port = port;
            outAddr = addr;
            found = true;
            break;
        }

        freeaddrinfo(results);
        return found;
    }

} // namespace

bool ClientNetwork::Start() {
    if (m_started.load()) {
        return true;
    }
    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        std::cerr << "GNS init failed: " << err << "\n";
        SetConnectionStatus(ConnectionState::Disconnected, "network init failed", false);
        return false;
    }
    m_started = true;
    SetConnectionStatus(ConnectionState::Disconnected, "network initialized", false);
    return true;
}

bool ClientNetwork::ConnectTo(std::string_view host, uint16_t port) {
    if (!m_started.load()) {
        std::cerr << "ClientNetwork: Start() must be called first\n";
        SetConnectionStatus(ConnectionState::Disconnected, "network not started");
        return false;
    }

    std::string hostTrimmed(host);
    size_t begin = 0;
    while (begin < hostTrimmed.size() &&
           std::isspace(static_cast<unsigned char>(hostTrimmed[begin])) != 0) {
        ++begin;
    }
    size_t end = hostTrimmed.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(hostTrimmed[end - 1])) != 0) {
        --end;
    }
    hostTrimmed = hostTrimmed.substr(begin, end - begin);
    if (hostTrimmed.empty()) {
        std::cerr << "ConnectTo: empty host\n";
        SetConnectionStatus(ConnectionState::Disconnected, "invalid server host");
        return false;
    }

    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "reconnect", false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    m_registered = false;
    m_assignedUsername.clear();
    m_allowAutoReconnect = true;
    m_chunkResyncOverflowCooldownUntil.clear();

    SteamNetworkingIPAddr addr;
    addr.Clear();
    if (!ResolveHostToAddress(hostTrimmed, port, addr)) {
        std::cerr << "ConnectTo: failed to resolve host '" << hostTrimmed << "'\n";
        SetConnectionStatus(ConnectionState::Disconnected, "failed to resolve server host");
        return false;
    }

    m_conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    if (m_conn == k_HSteamNetConnection_Invalid) {
        std::cerr << "ConnectByIPAddress failed\n";
        SetConnectionStatus(ConnectionState::Disconnected, "connect attempt failed");
        return false;
    }
    {
        std::ostringstream status;
        status << "connecting to " << hostTrimmed << ":" << port;
        SetConnectionStatus(ConnectionState::Connecting, status.str());
    }
    return true;
}

bool ClientNetwork::SetClientIdentityOverride(std::string_view identity) {
    std::string normalized = NormalizeIdentity(std::string(identity));
    if (!IsValidIdentity(normalized)) {
        return false;
    }

    m_clientIdentity = std::move(normalized);
    m_useTransientIdentity = true;
    return true;
}

void ClientNetwork::Shutdown() {
    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "client shutdown", false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    if (m_started.load()) {
        GameNetworkingSockets_Kill();
        m_started = false;
    }
    m_registered = false;
    m_assignedUsername.clear();
    m_hasEverConnectedSuccessfully = false;
    m_retryWithAutoAssignedUsername = false;
    m_chunkResyncOverflowCooldownUntil.clear();
    SetConnectionStatus(ConnectionState::Disconnected, "disconnected");

    {
        std::lock_guard<std::mutex> lk(m_inboundMutex);
        m_chunkDataQueue.clear();
        m_chunkDeltaQueue.clear();
        m_chunkUnloadQueue.clear();
        m_playerSnapshotQueue.clear();
        m_shootResultQueue.clear();
        m_inventoryActionResultQueue.clear();
        m_inventorySnapshotQueue.clear();
        m_worldItemSnapshotQueue.clear();
        m_blockPlaceResultQueue.clear();
        m_blockBreakResultQueue.clear();
        m_killFeedQueue.clear();
        m_scoreboardQueue.clear();
    }
}

void ClientNetwork::DisconnectFromServer() {
    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "leave game", false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    m_registered = false;
    m_assignedUsername.clear();
    m_hasEverConnectedSuccessfully = false;
    m_retryWithAutoAssignedUsername = false;
    m_allowAutoReconnect = false;
    m_chunkResyncOverflowCooldownUntil.clear();
    SetConnectionStatus(ConnectionState::Disconnected, "disconnected", false);

    {
        std::lock_guard<std::mutex> lk(m_inboundMutex);
        m_chunkDataQueue.clear();
        m_chunkDeltaQueue.clear();
        m_chunkUnloadQueue.clear();
        m_playerSnapshotQueue.clear();
        m_shootResultQueue.clear();
        m_inventoryActionResultQueue.clear();
        m_inventorySnapshotQueue.clear();
        m_worldItemSnapshotQueue.clear();
        m_blockPlaceResultQueue.clear();
        m_blockBreakResultQueue.clear();
        m_killFeedQueue.clear();
        m_scoreboardQueue.clear();
    }
}

bool ClientNetwork::IsConnected() const {
    if (!m_started.load() || m_conn == k_HSteamNetConnection_Invalid || !m_registered) {
        return false;
    }

    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(m_conn, &info)) {
        return false;
    }
    return info.m_eState == k_ESteamNetworkingConnectionState_Connected;
}

int ClientNetwork::GetPingMs() const noexcept {
    if (!IsConnected()) {
        return -1;
    }

    SteamNetConnectionRealTimeStatus_t status{};
    const EResult r =
        SteamNetworkingSockets()->GetConnectionRealTimeStatus(m_conn, &status, 0, nullptr);
    if (r != k_EResultOK) {
        return -1;
    }

    return status.m_nPing;
}

bool ClientNetwork::EnsureClientIdentity() {
    if (IsValidIdentity(m_clientIdentity)) {
        return true;
    }

    if (m_useTransientIdentity) {
        std::string transient = NormalizeIdentity(GenerateIdentityToken());
        if (!IsValidIdentity(transient)) {
            return false;
        }
        m_clientIdentity = std::move(transient);
        return true;
    }

    const std::filesystem::path identityPath = ResolveIdentityFilePath();
    std::error_code ec;
    if (!identityPath.parent_path().empty()) {
        std::filesystem::create_directories(identityPath.parent_path(), ec);
    }

    std::string loaded;
    {
        std::ifstream in(identityPath);
        if (in) {
            std::getline(in, loaded);
        }
    }
    loaded = NormalizeIdentity(std::move(loaded));
    if (!IsValidIdentity(loaded)) {
        loaded = NormalizeIdentity(GenerateIdentityToken());
    }
    if (!IsValidIdentity(loaded)) {
        return false;
    }

    {
        std::ofstream out(identityPath, std::ios::out | std::ios::trunc);
        if (out) {
            out << loaded << "\n";
        }
    }
    m_clientIdentity = loaded;
    return true;
}
