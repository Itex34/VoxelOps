#include "../Runtime.hpp"

Runtime *Runtime::s_instance = nullptr;

Runtime::Runtime()
    : m_quit(false), m_pollGroup(k_HSteamNetPollGroup_Invalid),
      m_listenSock(k_HSteamListenSocket_Invalid) {
    // allow only one instance to own the static callback bridge
    s_instance = this;
}

Runtime::~Runtime() {
    Stop();
    ShutdownNetworking();
    // cleanup pointer
    if (s_instance == this)
        s_instance = nullptr;
}

bool Runtime::Start(uint16_t port) {
    if (m_started.load(std::memory_order_acquire)) {
        std::cerr << "Network already started\n";
        return false;
    }

    m_quit.store(false, std::memory_order_release);
    m_serverTick.store(0, std::memory_order_release);
    m_lagCompFrames.clear();
    m_combatSnapshotsAliveCache.clear();
    m_combatSnapshotsAliveCacheTick = 0;
    m_hasCombatSnapshotsAliveCache = false;
    m_matchStartTime = std::chrono::steady_clock::now();
    m_matchStarted = false;
    m_matchEnded = false;
    m_matchWinner.clear();
    m_worldItems.clear();
    m_nextWorldItemId = 1;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_matchScores.clear();
        m_connectionByPlayerId.clear();
    }
    {
        std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
        m_shutdownComplete = false;
    }

    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        std::cerr << "GameNetworkingSockets_Init failed: " << err << "\n";
        return false;
    }

    LoadHistoryFromFile();
    LoadAdminsFromFile();

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
               reinterpret_cast<void *>(Runtime::SteamNetConnectionStatusChangedCallback));

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
    } else {
        std::cout << "Server listening on UDP port " << port << " (Ctrl+C to quit)\n";
    }

    StartChunkPipeline();
    m_started.store(true, std::memory_order_release);
    return true;
}

void Runtime::Run() {
    if (!m_started.load(std::memory_order_acquire)) {
        std::cerr << "Runtime::Run called before Start\n";
        return;
    }
    MainLoop();
    ShutdownNetworking();
}

void Runtime::Stop() {
    m_quit.store(true, std::memory_order_release);
}

void Runtime::ShutdownNetworking() {
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
    if (m_shutdownComplete) {
        return;
    }
    m_shutdownComplete = true;

    StopChunkPipeline();
    SaveHistoryToFile();
    SaveAdminsToFile();

    std::vector<std::pair<HSteamNetConnection, ClientSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        sessions.reserve(m_clients.size());
        for (const auto &kv : m_clients) {
            sessions.push_back(kv);
        }
        m_clients.clear();
        m_connectionByPlayerId.clear();
        m_matchScores.clear();
        m_worldItems.clear();
        m_nextWorldItemId = 1;
        m_combatSnapshotsAliveCache.clear();
        m_combatSnapshotsAliveCacheTick = 0;
        m_hasCombatSnapshotsAliveCache = false;
    }

    for (const auto &[conn, session] : sessions) {
        TeardownClientSession(conn, session, "server shutting down", true);
    }

    if (m_listenSock != k_HSteamListenSocket_Invalid) {
        SteamNetworkingSockets()->CloseListenSocket(m_listenSock);
        m_listenSock = k_HSteamListenSocket_Invalid;
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    if (m_started.exchange(false, std::memory_order_acq_rel)) {
        GameNetworkingSockets_Kill();
    }
}
