#include "ServerNetwork.hpp"




using namespace std;

ServerNetwork* ServerNetwork::s_instance = nullptr;

// Free function signal handler for SIGINT
static void GlobalSignalHandler(int) {
    if (ServerNetwork::s_instance) ServerNetwork::s_instance->Stop();
}

ServerNetwork::ServerNetwork()
    : m_quit(false),
    m_pollGroup(k_HSteamNetPollGroup_Invalid),
    m_listenSock(k_HSteamListenSocket_Invalid)
{
    // allow only one instance to own the static callback bridge
    s_instance = this;
}

ServerNetwork::~ServerNetwork()
{
    Stop();
    // cleanup pointer
    if (s_instance == this) s_instance = nullptr;
}

bool ServerNetwork::Start(uint16_t port)
{
    // install SIGINT handler
    std::signal(SIGINT, GlobalSignalHandler);

    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        std::cerr << "GameNetworkingSockets_Init failed: " << err << "\n";
        return false;
    }

    LoadHistoryFromFile();

    // Create poll group (used to efficiently receive messages from many connections)
    m_pollGroup = SteamNetworkingSockets()->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid) {
        std::cerr << "CreatePollGroup failed\n";
        GameNetworkingSockets_Kill();
        return false;
    }

    // Prepare listen socket option to install our connection-status callback
    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(ServerNetwork::SteamNetConnectionStatusChangedCallback));

    // Create listen socket bound to the chosen port
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;
    m_listenSock = SteamNetworkingSockets()->CreateListenSocketIP(addr, 1, &opt);
    if (m_listenSock == k_HSteamListenSocket_Invalid) {
        std::cerr << "CreateListenSocketIP failed\n";
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
        GameNetworkingSockets_Kill();
        return false;
    }

    // Print bound address for debugging
    SteamNetworkingIPAddr boundAddr;
    if (SteamNetworkingSockets()->GetListenSocketAddress(m_listenSock, &boundAddr)) {
        char s[SteamNetworkingIPAddr::k_cchMaxString];
        boundAddr.ToString(s, sizeof(s), true);
        std::cout << "Server listening on " << s << " (Ctrl+C to quit)\n";
    }
    else {
        std::cout << "Server listening on UDP port " << port << " (Ctrl+C to quit)\n";
    }

    return true;
}

void ServerNetwork::Run()
{
    MainLoop();
}

void ServerNetwork::Stop()
{
    m_quit = true;

    // Save history
    SaveHistoryToFile();

    // Close all connections and cleanup
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& kv : m_clients) {
            SteamNetworkingSockets()->CloseConnection(kv.first, 0, "server shutting down", false);
        }
        m_clients.clear();
    }

    if (m_listenSock != k_HSteamListenSocket_Invalid) {
        SteamNetworkingSockets()->CloseListenSocket(m_listenSock);
        m_listenSock = k_HSteamListenSocket_Invalid;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    GameNetworkingSockets_Kill();

    std::cout << "Server stopped\n";
}

// Little-endian readers used for compact binary packets
static uint32_t ReadUint32LE(const uint8_t* ptr) {
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}
static float ReadFloatLE(const uint8_t* ptr) {
    uint32_t u = ReadUint32LE(ptr);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

void ServerNetwork::MainLoop()
{
    while (!m_quit) {
        SteamNetworkingSockets()->RunCallbacks();

        // Receive messages on poll group (any connection assigned to it)
        SteamNetworkingMessage_t* pMsg = nullptr;
        int n = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, &pMsg, 1);
        if (n > 0 && pMsg) {
            HSteamNetConnection incoming = pMsg->m_conn;
            const void* data = pMsg->m_pData;
            uint32_t cb = pMsg->m_cbSize;
            uint8_t t = (cb >= 1) ? reinterpret_cast<const uint8_t*>(data)[0] : 0;

            if (static_cast<PacketType>(t) == PacketType::ConnectRequest) {
                std::string username = ReadStringFromPacket(data, cb, 1);
                bool ok = true;

                if (username.empty() || username.size() > 64) ok = false;

                // uniqueness
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    for (auto& kv : m_clients) {
                        if (!kv.second.empty() && kv.second == username) { ok = false; break; }
                    }
                    if (ok) m_clients[incoming] = username;
                }
                char resp[2] = { static_cast<char>(PacketType::ConnectResponse), ok ? 1 : 0 };
                SteamNetworkingSockets()->SendMessageToConnection(incoming, resp, sizeof(resp), k_nSteamNetworkingSend_Reliable, nullptr);
                if (ok) {
                    std::string out;
                    out.push_back(static_cast<char>(PacketType::ClientConnect));
                    out += username;
                    BroadcastRaw(out.data(), (uint32_t)out.size(), incoming);
                    std::cout << "[register] conn=" << incoming << " username=" << username << "\n";
                }
                else {
                    std::cout << "[register rejected] conn=" << incoming << " username=" << username << "\n";
                }
            }
            else if (static_cast<PacketType>(t) == PacketType::Message) {
                std::string msg = ReadStringFromPacket(data, cb, 1);
                std::string username;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    auto it = m_clients.find(incoming);
                    if (it != m_clients.end()) username = it->second;
                }
                if (!username.empty()) {
                    m_messageHistory.emplace_back(username, msg);
                    std::string out;
                    out.push_back(static_cast<char>(PacketType::Message));
                    out += username;
                    out.push_back(':');
                    out += msg;
                    BroadcastRaw(out.data(), (uint32_t)out.size(), incoming);
                    std::cout << "[recv] " << username << ": " << msg << "\n";
                }
                else {
                    std::cout << "[dropping] message from unregistered conn=" << incoming << "\n";
                }
            }
            else if (static_cast<PacketType>(t) == PacketType::PlayerPosition) {
                // Expect: [1 byte type][4 bytes seq][6 floats: px,py,pz,vx,vy,vz] -> total 1 + 4 + 24 = 29 bytes
                if (cb < 1 + 4 + 6 * 4) {
                    std::cout << "[recv] malformed PlayerPosition (size=" << cb << ")\n";
                }
                else {
                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
                    size_t off = 1;
                    uint32_t seq = ReadUint32LE(bytes + off); off += 4;
                    float px = ReadFloatLE(bytes + off); off += 4;
                    float py = ReadFloatLE(bytes + off); off += 4;
                    float pz = ReadFloatLE(bytes + off); off += 4;
                    float vx = ReadFloatLE(bytes + off); off += 4;
                    float vy = ReadFloatLE(bytes + off); off += 4;
                    float vz = ReadFloatLE(bytes + off); off += 4;

                    std::string username;
                    {
                        std::lock_guard<std::mutex> lk(m_mutex);
                        auto it = m_clients.find(incoming);
                        if (it != m_clients.end()) username = it->second;
                    }
                    if (!username.empty()) {
                        std::cout << "[pos] user = " << username
                            << " seq = " << seq
                            << " pos = (" << px << "," << py << "," << pz << ")"
                            << " vel = (" << vx << "," << vy << "," << vz << ")\n";
                    }
                    else {
                        std::cout << "[pos] unregistered conn = " << incoming << " seq = " << seq << "\n";
                    }
                }
            }
            else if (static_cast<PacketType>(t) == PacketType::ShootRequest) {
                // copy incoming bytes into vector for deserialization
                std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + cb);
                auto optReq = ShootRequest::deserialize(buf);
                if (!optReq.has_value()) {
                    std::cerr << "[recv] malformed ShootRequest\n";
                    pMsg->Release();
                    continue;
                }
                ShootRequest req = *optReq;

                std::string username;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    auto it = m_clients.find(incoming);
                    if (it != m_clients.end()) username = it->second;
                }

                if (username.empty()) {
                    std::cout << "[recv] ShootRequest from unregistered conn = " << incoming << "\n";
                    pMsg->Release();
                    continue;
                }

                // Example server-side logic: validate shot, check ammo, do hit detection
                ShootResult res;
                res.clientShotId = req.clientShotId;
                res.didHit = true; // or result of hit detection
                res.hitEntityId = 123;  // example
                res.hitX = req.posX + req.dirX;  // example hit position
                res.hitY = req.posY + req.dirY;
                res.hitZ = req.posZ + req.dirZ;
                res.damageApplied = 25; // example
                res.accepted = true; // reject if invalid
                res.newAmmoCount = 9;    // example ammo reconciliation

                // Serialize and send back to client
                std::vector<uint8_t> outBuf = res.serialize();
                SteamNetworkingSockets()->SendMessageToConnection(incoming, outBuf.data(), (uint32_t)outBuf.size(), k_nSteamNetworkingSend_Reliable, nullptr);

                // Optionally broadcast hit effects to other clients if needed
                pMsg->Release();
                continue;
            }



            pMsg->Release();
            continue;
        }

        // Optional: extra safeguard - check connection states for any connections left (callback already handles most)
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_clients.begin(); it != m_clients.end();) {
                HSteamNetConnection conn = it->first;
                SteamNetConnectionInfo_t info;
                if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info)) {
                    if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                        std::string username = it->second;
                        std::cout << "[cleanup] remove conn=" << conn << " user=" << username << "\n";
                        std::string out;
                        out.push_back(static_cast<char>(PacketType::ClientDisconnect));
                        out += username;
                        BroadcastRaw(out.data(), (uint32_t)out.size(), conn);
                        SteamNetworkingSockets()->CloseConnection(conn, 0, "server cleanup", false);
                        it = m_clients.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Broadcast raw payload to everyone except `except`
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
            m_clients.emplace(hConn, std::string()); // username empty until client sends ConnectRequest
        }
        std::cout << "[callback] accepted conn=" << hConn << "\n";
        return;
    }

    if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        // Remove and notify
        std::string username;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(hConn);
            if (it != m_clients.end()) {
                username = it->second;
                m_clients.erase(it);
            }
        }
        if (!username.empty()) {
            std::string out;
            out.push_back(static_cast<char>(PacketType::ClientDisconnect));
            out += username;
            BroadcastRaw(out.data(), (uint32_t)out.size(), hConn);
        }
        std::cout << "[callback] conn closed/failed: conn=" << hConn << " reason=" << info.m_eState << "\n";
        // Safe to CloseConnection here if you want:
        SteamNetworkingSockets()->CloseConnection(hConn, 0, "closed by server callback", false);
        return;
    }

    // other states (Connected, FindingRoute, etc.) can be logged if desired
}

void ServerNetwork::SaveHistoryToFile()
{
    std::ofstream fout(HISTORY_FILE, std::ios::out | std::ios::trunc);
    if (!fout) return;
    for (auto& m : m_messageHistory) {
        std::string msg = m.second;
        std::replace(msg.begin(), msg.end(), '\n', ' ');
        fout << m.first << ':' << msg << '\n';
    }
}

void ServerNetwork::LoadHistoryFromFile() {
    std::ifstream fin(HISTORY_FILE);
    if (!fin) return;
    m_messageHistory.clear();
    std::string line;
    while (std::getline(fin, line)) {
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string user = line.substr(0, pos);
        std::string msg = line.substr(pos + 1);
        m_messageHistory.emplace_back(user, msg);
    }
}


