#include "ServerNetwork.hpp"

void ServerNetwork::BroadcastRaw(const void* data, uint32_t len, HSteamNetConnection except)
{
    std::vector<HSteamNetConnection> copy;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        copy.reserve(m_clients.size());
        for (auto& kv : m_clients) copy.push_back(kv.first);
    }
    for (auto c : copy) {
        if (c == except) continue;
        SteamNetworkingSockets()->SendMessageToConnection(c, data, len, k_nSteamNetworkingSend_Reliable, nullptr);
    }
}

// helper: read string payload from a message where first byte is packet type
std::string ServerNetwork::ReadStringFromPacket(const void* data, uint32_t size, size_t offset)
{
    if (size <= offset) return {};
    const char* bytes = reinterpret_cast<const char*>(data);
    return std::string(bytes + offset, size - offset);
}

// Static bridge called by Steam networking when connection state changes.
// Delegates to the instance method if available.
void ServerNetwork::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    if (s_instance) s_instance->OnConnectionStatusChanged(pInfo);
}

void ServerNetwork::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    if (!pInfo) return;

    HSteamNetConnection hConn = pInfo->m_hConn;
    const SteamNetConnectionInfo_t& info = pInfo->m_info;

    if (info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
        // Incoming connection attempt -> accept and assign to poll group
        EResult res = SteamNetworkingSockets()->AcceptConnection(hConn);
        if (res != k_EResultOK) {
            std::cerr << "[callback] AcceptConnection failed: " << res << " conn=" << hConn << "\n";
            return;
        }

        // Add to poll group so we can ReceiveMessagesOnPollGroup
        if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
            bool ok = SteamNetworkingSockets()->SetConnectionPollGroup(hConn, m_pollGroup);
            if (!ok) {
                std::cerr << "[callback] SetConnectionPollGroup failed for conn=" << hConn << "\n";
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_clients.emplace(hConn, ClientSession{}); // username empty until client sends ConnectRequest
        }
        std::cout << "[callback] accepted conn=" << hConn << "\n";
        return;
    }

    if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        // Remove and notify
        ClientSession session{};
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(hConn);
            if (it != m_clients.end()) {
                session = it->second;
                m_clients.erase(it);
            }
        }
        if (session.playerId != 0) {
            m_playerManager.removePlayer(session.playerId);
        }
        ClearChunkPipelineForConnection(hConn);
        if (!session.username.empty()) {
            std::string out;
            out.push_back(static_cast<char>(PacketType::ClientDisconnect));
            out += session.username;
            BroadcastRaw(out.data(), (uint32_t)out.size(), hConn);
        }
        std::cout << "[callback] conn closed/failed: conn=" << hConn << " reason=" << info.m_eState << "\n";
        // Safe to CloseConnection here if you want:
        SteamNetworkingSockets()->CloseConnection(hConn, 0, "closed by server callback", false);
        return;
    }

    // other states (Connected, FindingRoute, etc.) can be logged if desired
}

