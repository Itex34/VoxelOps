#include "ServerNetwork.hpp"
#include "CompressChunk.hpp"



#include <glm/vec3.hpp>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>


using namespace std;

ServerNetwork* ServerNetwork::s_instance = nullptr;

namespace {
uint32_t fnv1a32(const uint8_t* data, size_t size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint32_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}
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
    ShutdownNetworking();
    // cleanup pointer
    if (s_instance == this) s_instance = nullptr;
}

bool ServerNetwork::Start(uint16_t port)
{
    if (m_started.load(std::memory_order_acquire)) {
        std::cerr << "ServerNetwork already started\n";
        return false;
    }

    m_quit.store(false, std::memory_order_release);
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

    StartChunkPipeline();
    m_started.store(true, std::memory_order_release);
    return true;
}

void ServerNetwork::Run()
{
    if (!m_started.load(std::memory_order_acquire)) {
        std::cerr << "ServerNetwork::Run called before Start\n";
        return;
    }
    MainLoop();
    ShutdownNetworking();
}

void ServerNetwork::Stop()
{
    m_quit.store(true, std::memory_order_release);
}

void ServerNetwork::ShutdownNetworking()
{
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
    if (m_shutdownComplete) {
        return;
    }
    m_shutdownComplete = true;

    StopChunkPipeline();
    SaveHistoryToFile();

    std::vector<std::pair<HSteamNetConnection, ClientSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        sessions.reserve(m_clients.size());
        for (const auto& kv : m_clients) {
            sessions.push_back(kv);
        }
        m_clients.clear();
    }

    for (const auto& [conn, session] : sessions) {
        ClearChunkPipelineForConnection(conn);
        if (session.playerId != 0) {
            m_playerManager.removePlayer(session.playerId);
        }
        SteamNetworkingSockets()->CloseConnection(conn, 0, "server shutting down", false);
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

static int FloorDiv(int a, int b) {
    int q = a / b;
    const int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

uint16_t ServerNetwork::ClampViewDistance(uint16_t requested)
{
    constexpr uint16_t kMin = 2;
    const int spanX = WORLD_MAX_X - WORLD_MIN_X;
    const int spanZ = WORLD_MAX_Z - WORLD_MIN_Z;
    const int diagonalRadius = static_cast<int>(
        std::ceil(std::sqrt(static_cast<double>(spanX * spanX + spanZ * spanZ)))
    );
    const uint16_t kMax = static_cast<uint16_t>(
        std::max(
            static_cast<int>(kMin),
            diagonalRadius
        )
    );
    return std::clamp(requested, kMin, kMax);
}

std::string ServerNetwork::AllocateAutoUsernameLocked(HSteamNetConnection incomingConn)
{
    constexpr uint32_t kNameSpaceSize = 10000;
    for (uint32_t attempt = 0; attempt < kNameSpaceSize; ++attempt) {
        const uint32_t suffix = (m_nextAutoUsername + attempt) % kNameSpaceSize;
        std::ostringstream oss;
        oss << "player#" << std::setfill('0') << std::setw(4) << suffix;
        const std::string candidate = oss.str();

        bool taken = false;
        for (const auto& [conn, session] : m_clients) {
            if (conn == incomingConn || session.username.empty()) {
                continue;
            }
            if (session.username == candidate) {
                taken = true;
                break;
            }
        }

        if (!taken) {
            m_nextAutoUsername = (suffix + 1) % kNameSpaceSize;
            return candidate;
        }
    }

    return {};
}

void ServerNetwork::StartChunkPipeline()
{
    {
        std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);
        m_chunkPrepQueue.clear();
        m_chunkPrepQueued.clear();
        m_chunkSendQueues.clear();
        m_chunkSendQueued.clear();
    }
    m_chunkPrepQuit.store(false, std::memory_order_release);
    if (!m_chunkPrepThread.joinable()) {
        m_chunkPrepThread = std::thread([this]() { ChunkPrepWorkerLoop(); });
    }
}

void ServerNetwork::StopChunkPipeline()
{
    m_chunkPrepQuit.store(true, std::memory_order_release);
    m_chunkPrepCv.notify_all();
    if (m_chunkPrepThread.joinable()) {
        m_chunkPrepThread.join();
    }
    {
        std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);
        m_chunkPrepQueue.clear();
        m_chunkPrepQueued.clear();
        m_chunkSendQueues.clear();
        m_chunkSendQueued.clear();
    }
    m_chunkPrepQuit.store(false, std::memory_order_release);
}

bool ServerNetwork::PrepareChunkForStreaming(const ChunkCoord& coord)
{
    // Materialize neighborhood off the network thread so chunk sends stay lightweight.
    constexpr int kDecorationNeighborRadiusXZ = 1;
    constexpr int kDecorationNeighborRadiusY = 1;
    for (int dx = -kDecorationNeighborRadiusXZ; dx <= kDecorationNeighborRadiusXZ; ++dx) {
        for (int dz = -kDecorationNeighborRadiusXZ; dz <= kDecorationNeighborRadiusXZ; ++dz) {
            for (int dy = -kDecorationNeighborRadiusY; dy <= kDecorationNeighborRadiusY; ++dy) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                const glm::ivec3 npos(coord.x + dx, coord.y + dy, coord.z + dz);
                if (!m_chunkManager.inBounds(npos)) {
                    continue;
                }
                (void)m_chunkManager.loadOrGenerateChunk(npos);
            }
        }
    }

    ServerChunk* chunk = m_chunkManager.loadOrGenerateChunk(glm::ivec3(coord.x, coord.y, coord.z));
    return chunk != nullptr;
}

void ServerNetwork::ChunkPrepWorkerLoop()
{
    while (true) {
        ChunkPrepTask task;
        {
            std::unique_lock<std::mutex> lk(m_chunkPipelineMutex);
            m_chunkPrepCv.wait(lk, [this]() {
                return m_chunkPrepQuit.load(std::memory_order_acquire) || !m_chunkPrepQueue.empty();
            });
            if (m_chunkPrepQuit.load(std::memory_order_acquire) && m_chunkPrepQueue.empty()) {
                return;
            }
            task = m_chunkPrepQueue.front();
            m_chunkPrepQueue.pop_front();
        }

        bool stillNeeded = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(task.conn);
            if (it != m_clients.end()) {
                stillNeeded = it->second.pendingChunkData.find(task.coord) != it->second.pendingChunkData.end();
            }
        }

        const bool prepared = stillNeeded && PrepareChunkForStreaming(task.coord);
        const ChunkPipelineKey key{ task.conn, task.coord };
        {
            std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);
            m_chunkPrepQueued.erase(key);
            if (
                prepared &&
                !m_chunkPrepQuit.load(std::memory_order_acquire) &&
                m_chunkSendQueued.find(key) == m_chunkSendQueued.end()
            ) {
                auto& sendQ = m_chunkSendQueues[task.conn];
                if (sendQ.size() < kMaxChunkSendQueuePerClient) {
                    sendQ.push_back(task.coord);
                    m_chunkSendQueued.insert(key);
                }
            }
        }
    }
}

bool ServerNetwork::QueueChunkPreparation(HSteamNetConnection conn, const ChunkCoord& coord)
{
    const ChunkPipelineKey key{ conn, coord };
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);
        if (m_chunkPrepQueued.find(key) != m_chunkPrepQueued.end() ||
            m_chunkSendQueued.find(key) != m_chunkSendQueued.end()) {
            return true;
        }
        if (m_chunkPrepQueue.size() >= kMaxChunkPrepQueue) {
            return false;
        }
        m_chunkPrepQueue.push_back(ChunkPrepTask{ conn, coord });
        m_chunkPrepQueued.insert(key);
        queued = true;
    }
    if (queued) {
        m_chunkPrepCv.notify_one();
    }
    return queued;
}

size_t ServerNetwork::FlushChunkSendQueueForClient(HSteamNetConnection conn, size_t maxSends)
{
    size_t sent = 0;
    while (sent < maxSends) {
        ChunkCoord coord{};
        bool haveChunk = false;
        {
            std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);
            auto qIt = m_chunkSendQueues.find(conn);
            if (qIt == m_chunkSendQueues.end() || qIt->second.empty()) {
                break;
            }
            coord = qIt->second.front();
            qIt->second.pop_front();
            if (qIt->second.empty()) {
                m_chunkSendQueues.erase(qIt);
            }
            m_chunkSendQueued.erase(ChunkPipelineKey{ conn, coord });
            haveChunk = true;
        }
        if (!haveChunk) {
            break;
        }

        bool stillPending = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(conn);
            if (it != m_clients.end()) {
                stillPending = it->second.pendingChunkData.find(coord) != it->second.pendingChunkData.end();
            }
        }
        if (!stillPending) {
            continue;
        }

        uint32_t payloadHash = 0;
        if (!SendChunkData(conn, coord, &payloadHash)) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(conn);
            if (it != m_clients.end() && it->second.pendingChunkData.find(coord) != it->second.pendingChunkData.end()) {
                it->second.pendingChunkData[coord] = now;
                it->second.pendingChunkDataPayloadHash[coord] = payloadHash;
            }
        }
        ++sent;
    }
    return sent;
}

size_t ServerNetwork::GetChunkSendQueueDepthForClient(HSteamNetConnection conn)
{
    std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);
    auto it = m_chunkSendQueues.find(conn);
    if (it == m_chunkSendQueues.end()) {
        return 0;
    }
    return it->second.size();
}

void ServerNetwork::ClearChunkPipelineForConnection(HSteamNetConnection conn)
{
    std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);

    m_chunkSendQueues.erase(conn);

    for (auto it = m_chunkPrepQueue.begin(); it != m_chunkPrepQueue.end();) {
        if (it->conn == conn) {
            it = m_chunkPrepQueue.erase(it);
        }
        else {
            ++it;
        }
    }

    for (auto it = m_chunkPrepQueued.begin(); it != m_chunkPrepQueued.end();) {
        if (it->conn == conn) {
            it = m_chunkPrepQueued.erase(it);
        }
        else {
            ++it;
        }
    }

    for (auto it = m_chunkSendQueued.begin(); it != m_chunkSendQueued.end();) {
        if (it->conn == conn) {
            it = m_chunkSendQueued.erase(it);
        }
        else {
            ++it;
        }
    }
}

bool ServerNetwork::SendChunkData(HSteamNetConnection conn, const ChunkCoord& coord, uint32_t* outPayloadHash)
{
    ServerChunk* chunk = m_chunkManager.getChunkIfExists(glm::ivec3(coord.x, coord.y, coord.z));
    if (!chunk) {
        std::cerr
            << "[chunk/send] chunk missing after prep for conn=" << conn
            << " chunk=(" << coord.x << "," << coord.y << "," << coord.z << ")\n";
        return false;
    }

    ChunkData packet;
    packet.chunkX = coord.x;
    packet.chunkY = coord.y;
    packet.chunkZ = coord.z;
    packet.version = static_cast<uint64_t>(std::max<int64_t>(0, chunk->version()));
    const std::vector<uint8_t> rawPayload = chunk->serializeCompressed();
    const CompressedChunkPayload compressedPayload = CompressChunkPayload(rawPayload);
    packet.flags = compressedPayload.compressed ? 0x1u : 0u;
    packet.payload = compressedPayload.payload;
    if (outPayloadHash) {
        *outPayloadHash = fnv1a32(packet.payload.data(), packet.payload.size());
    }

    const std::vector<uint8_t> bytes = packet.serialize();
    const EResult result = SteamNetworkingSockets()->SendMessageToConnection(
        conn,
        bytes.data(),
        static_cast<uint32_t>(bytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    if (result != k_EResultOK) {
        SteamNetConnectionInfo_t info{};
        const bool haveInfo = SteamNetworkingSockets()->GetConnectionInfo(conn, &info);
        std::cerr
            << "[chunk/send] SendMessageToConnection failed result=" << result
            << " conn=" << conn
            << " chunk=(" << coord.x << "," << coord.y << "," << coord.z << ")"
            << " bytes=" << bytes.size();
        if (haveInfo) {
            std::cerr << " connState=" << info.m_eState;
        }
        std::cerr << "\n";
    }
    return result == k_EResultOK;
}

bool ServerNetwork::SendChunkUnload(HSteamNetConnection conn, const ChunkCoord& coord)
{
    ChunkUnload packet;
    packet.chunkX = coord.x;
    packet.chunkY = coord.y;
    packet.chunkZ = coord.z;

    const std::vector<uint8_t> bytes = packet.serialize();
    const EResult result = SteamNetworkingSockets()->SendMessageToConnection(
        conn,
        bytes.data(),
        static_cast<uint32_t>(bytes.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return result == k_EResultOK;
}

void ServerNetwork::UpdateChunkStreamingForClient(HSteamNetConnection conn, const glm::ivec3& centerChunk, uint16_t viewDistance)
{
    constexpr size_t kMaxChunkSendsPerUpdate = 24;
    constexpr size_t kMaxPendingChunkData = 128;
    constexpr auto kChunkRetryInterval = std::chrono::milliseconds(500);
    const uint16_t clampedViewDistance = ClampViewDistance(viewDistance);
    const auto now = std::chrono::steady_clock::now();

    std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
    const int minChunkY = FloorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = FloorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    const int radius = static_cast<int>(clampedViewDistance);
    const int64_t radius2 = static_cast<int64_t>(radius) * static_cast<int64_t>(radius);
    desired.reserve(static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1) * (maxChunkY - minChunkY + 1)));

    for (int x = centerChunk.x - radius; x <= centerChunk.x + radius; ++x) {
        const int64_t dx = static_cast<int64_t>(x - centerChunk.x);
        const int64_t dx2 = dx * dx;
        for (int z = centerChunk.z - radius; z <= centerChunk.z + radius; ++z) {
            const int64_t dz = static_cast<int64_t>(z - centerChunk.z);
            if (dx2 + dz * dz > radius2) {
                continue;
            }
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                glm::ivec3 pos(x, y, z);
                if (!m_chunkManager.inBounds(pos)) {
                    continue;
                }
                desired.insert(ChunkCoord{ x, y, z });
            }
        }
    }

    std::unordered_set<ChunkCoord, ChunkCoordHash> currentlyStreamed;
    std::unordered_set<ChunkCoord, ChunkCoordHash> pendingPossiblySent;
    std::unordered_map<ChunkCoord, std::chrono::steady_clock::time_point, ChunkCoordHash> pendingChunkData;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(conn);
        if (it == m_clients.end()) {
            return;
        }

        it->second.interestCenterChunk = centerChunk;
        it->second.viewDistance = clampedViewDistance;
        it->second.hasChunkInterest = true;

        currentlyStreamed = it->second.streamedChunks;
        pendingPossiblySent.reserve(it->second.pendingChunkData.size());
        for (const auto& entry : it->second.pendingChunkData) {
            pendingPossiblySent.insert(entry.first);
        }

        for (auto pIt = it->second.pendingChunkData.begin(); pIt != it->second.pendingChunkData.end();) {
            if (desired.find(pIt->first) == desired.end()) {
                it->second.pendingChunkDataPayloadHash.erase(pIt->first);
                pIt = it->second.pendingChunkData.erase(pIt);
            }
            else {
                ++pIt;
            }
        }

        pendingChunkData = it->second.pendingChunkData;
    }

    std::vector<ChunkCoord> toLoad;
    toLoad.reserve(desired.size());
    for (const ChunkCoord& c : desired) {
        if (currentlyStreamed.find(c) != currentlyStreamed.end()) {
            continue;
        }

        auto pendingIt = pendingChunkData.find(c);
        if (pendingIt != pendingChunkData.end()) {
            if ((now - pendingIt->second) < kChunkRetryInterval) {
                continue;
            }
        }

        toLoad.push_back(c);
    }

    const bool isInitialSync = currentlyStreamed.empty();
    int verticalAnchorY = std::clamp(centerChunk.y, minChunkY, maxChunkY);
    if (verticalAnchorY == maxChunkY && maxChunkY > minChunkY) {
        // Top-most chunk layers are often sparse; bias one layer down to prioritize terrain.
        --verticalAnchorY;
    }
    std::sort(toLoad.begin(), toLoad.end(), [&](const ChunkCoord& a, const ChunkCoord& b) {
        const int adx = a.x - centerChunk.x;
        const int adz = a.z - centerChunk.z;
        const int bdx = b.x - centerChunk.x;
        const int bdz = b.z - centerChunk.z;
        const int aHorizDist2 = adx * adx + adz * adz;
        const int bHorizDist2 = bdx * bdx + bdz * bdz;
        if (aHorizDist2 != bHorizDist2) {
            return aHorizDist2 < bHorizDist2;
        }

        if (isInitialSync) {
            const bool aUnderOrSame = (a.y <= verticalAnchorY);
            const bool bUnderOrSame = (b.y <= verticalAnchorY);
            if (aUnderOrSame != bUnderOrSame) {
                return aUnderOrSame;
            }
        }

        const int aVert = std::abs(a.y - verticalAnchorY);
        const int bVert = std::abs(b.y - verticalAnchorY);
        if (aVert != bVert) {
            return aVert < bVert;
        }

        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });

    std::unordered_set<ChunkCoord, ChunkCoordHash> toUnloadSet;
    toUnloadSet.reserve(currentlyStreamed.size() + pendingPossiblySent.size());
    for (const ChunkCoord& c : currentlyStreamed) {
        if (desired.find(c) == desired.end()) {
            toUnloadSet.insert(c);
        }
    }
    for (const ChunkCoord& c : pendingPossiblySent) {
        if (desired.find(c) == desired.end()) {
            toUnloadSet.insert(c);
        }
    }
    std::vector<ChunkCoord> toUnload;
    toUnload.reserve(toUnloadSet.size());
    for (const ChunkCoord& c : toUnloadSet) {
        toUnload.push_back(c);
    }

    size_t pendingCount = pendingChunkData.size();
    size_t queuedPrepThisUpdate = 0;
    size_t sentThisUpdate = 0;
    bool stoppedByPendingCap = false;
    bool stoppedByPrepCap = false;
    for (const ChunkCoord& c : toLoad) {
        const bool isRetry = pendingChunkData.find(c) != pendingChunkData.end();
        if (queuedPrepThisUpdate >= kMaxChunkSendsPerUpdate) {
            break;
        }
        if (!isRetry && pendingCount >= kMaxPendingChunkData) {
            stoppedByPendingCap = true;
            break;
        }
        if (!QueueChunkPreparation(conn, c)) {
            stoppedByPrepCap = true;
            break;
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(conn);
            if (it != m_clients.end()) {
                // Mark as pending as soon as chunk work is queued to enforce backpressure.
                const bool wasPending = it->second.pendingChunkData.find(c) != it->second.pendingChunkData.end();
                it->second.pendingChunkData[c] = now;
                if (!wasPending) {
                    ++pendingCount;
                }
            }
        }
        ++queuedPrepThisUpdate;
    }

    sentThisUpdate = FlushChunkSendQueueForClient(conn, kMaxChunkSendsPerUpdate);
    const size_t sendQueueDepth = GetChunkSendQueueDepthForClient(conn);

    static std::unordered_map<HSteamNetConnection, std::chrono::steady_clock::time_point> s_lastProgressLog;
    auto& lastLog = s_lastProgressLog[conn];
    if ((now - lastLog) >= std::chrono::seconds(1)) {
        lastLog = now;
        std::cerr
            << "[chunk/stream] progress conn=" << conn
            << " desired=" << desired.size()
            << " streamed=" << currentlyStreamed.size()
            << " pending=" << pendingCount
            << " toLoad=" << toLoad.size()
            << " queuedPrepNow=" << queuedPrepThisUpdate
            << " sentNow=" << sentThisUpdate
            << " pendingCapHit=" << (stoppedByPendingCap ? 1 : 0)
            << " prepCapHit=" << (stoppedByPrepCap ? 1 : 0)
            << " sendQueue=" << sendQueueDepth
            << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
            << " viewDist=" << clampedViewDistance << "\n";
    }

    if (!toLoad.empty() && queuedPrepThisUpdate == 0 && sentThisUpdate == 0) {
        std::cerr
            << "[chunk/stream] stalled load window conn=" << conn
            << " desired=" << desired.size()
            << " toLoad=" << toLoad.size()
            << " streamed=" << currentlyStreamed.size()
            << " pending=" << pendingCount
            << " pendingCap=" << kMaxPendingChunkData
            << " prepQueueCap=" << kMaxChunkPrepQueue
            << " sendQueue=" << sendQueueDepth
            << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
            << " viewDist=" << clampedViewDistance << "\n";
    }

    for (const ChunkCoord& c : toUnload) {
        if (!SendChunkUnload(conn, c)) {
            continue;
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(conn);
        if (it != m_clients.end()) {
            it->second.streamedChunks.erase(c);
            it->second.pendingChunkDataPayloadHash.erase(c);
            it->second.pendingChunkData.erase(c);
        }
    }
}

void ServerNetwork::MainLoop()
{
    auto lastFrameTime = std::chrono::steady_clock::now();
    auto lastSnapshotTime = lastFrameTime;
    constexpr auto snapshotInterval = std::chrono::milliseconds(100);

    while (!m_quit) {
        const auto frameNow = std::chrono::steady_clock::now();
        const double deltaSeconds = std::chrono::duration<double>(frameNow - lastFrameTime).count();
        lastFrameTime = frameNow;
        m_playerManager.update(deltaSeconds);

        SteamNetworkingSockets()->RunCallbacks();

        // Receive messages on poll group (any connection assigned to it).
        // Drain all available messages each tick to avoid ACK/request backlogs.
        SteamNetworkingMessage_t* pMsg = nullptr;
        while (SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, &pMsg, 1) > 0 && pMsg) {
            HSteamNetConnection incoming = pMsg->m_conn;
            const void* data = pMsg->m_pData;
            uint32_t cb = pMsg->m_cbSize;
            uint8_t t = (cb >= 1) ? reinterpret_cast<const uint8_t*>(data)[0] : 0;

            if (static_cast<PacketType>(t) == PacketType::ConnectRequest) {
                const std::string requestedUsername = ReadStringFromPacket(data, cb, 1);
                std::string username;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    username = AllocateAutoUsernameLocked(incoming);
                }
                bool ok = !username.empty();

                PlayerID playerId = 0;
                if (ok) {
                    auto connHandle = std::make_shared<ConnectionHandle>();
                    connHandle->socketFd = static_cast<int>(incoming);
                    playerId = m_playerManager.onPlayerConnect(connHandle, glm::vec3(0.0f, 60.0f, 0.0f));

                    bool attached = false;
                    {
                        std::lock_guard<std::mutex> lk(m_mutex);
                        auto it = m_clients.find(incoming);
                        if (it != m_clients.end()) {
                            it->second.username = username;
                            it->second.playerId = playerId;
                            attached = true;
                        }
                    }
                    if (!attached) {
                        m_playerManager.removePlayer(playerId);
                        playerId = 0;
                        ok = false;
                    }
                }

                char resp[2] = { static_cast<char>(PacketType::ConnectResponse), ok ? 1 : 0 };
                SteamNetworkingSockets()->SendMessageToConnection(incoming, resp, sizeof(resp), k_nSteamNetworkingSend_Reliable, nullptr);
                if (ok) {
                    std::string out;
                    out.push_back(static_cast<char>(PacketType::ClientConnect));
                    out += username;
                    BroadcastRaw(out.data(), (uint32_t)out.size(), incoming);
                    std::cout
                        << "[register] conn=" << incoming
                        << " username=" << username
                        << " requested=" << requestedUsername << "\n";
                }
                else {
                    std::cout
                        << "[register rejected] conn=" << incoming
                        << " requested=" << requestedUsername << "\n";
                }
            }
            else if (static_cast<PacketType>(t) == PacketType::Message) {
                std::string msg = ReadStringFromPacket(data, cb, 1);
                std::string username;
                PlayerID playerId = 0;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    auto it = m_clients.find(incoming);
                    if (it != m_clients.end()) {
                        username = it->second.username;
                        playerId = it->second.playerId;
                    }
                }
                if (!username.empty()) {
                    if (playerId != 0) {
                        m_playerManager.touchHeartbeat(playerId);
                    }
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
                    PlayerID playerId = 0;
                    {
                        std::lock_guard<std::mutex> lk(m_mutex);
                        auto it = m_clients.find(incoming);
                        if (it != m_clients.end()) {
                            username = it->second.username;
                            playerId = it->second.playerId;
                        }
                    }
                    if (!username.empty() && playerId != 0) {
                        m_playerManager.applyAuthoritativeState(
                            playerId,
                            glm::vec3(px, py, pz),
                            glm::vec3(vx, vy, vz)
                        );
                        std::cout << "[pos] user = " << username
                            << " seq = " << seq
                            << " pos = (" << px << "," << py << "," << pz << ")"
                            << " vel = (" << vx << "," << vy << "," << vz << ")\n";

                        // Chunk streaming is driven by explicit ChunkRequest packets.
                        // Avoid double-driving streaming here to reduce burst pressure.
                    }
                    else {
                        std::cout << "[pos] unregistered conn = " << incoming << " seq = " << seq << "\n";
                    }
                }
            }
            else if (static_cast<PacketType>(t) == PacketType::ChunkRequest) {
                std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + cb);
                const auto req = ChunkRequest::deserialize(buf);
                if (!req.has_value()) {
                    std::cout << "[recv] malformed ChunkRequest (size=" << cb << ")\n";
                    pMsg->Release();
                    continue;
                }

                bool registered = false;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    auto it = m_clients.find(incoming);
                    registered = (it != m_clients.end() && !it->second.username.empty() && it->second.playerId != 0);
                }
                if (!registered) {
                    pMsg->Release();
                    continue;
                }

                const glm::ivec3 centerChunk(req->chunkX, req->chunkY, req->chunkZ);
                UpdateChunkStreamingForClient(incoming, centerChunk, req->viewDistance);
            }
            else if (static_cast<PacketType>(t) == PacketType::ChunkAck) {
                std::vector<uint8_t> buf(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + cb);
                const auto ack = ChunkAck::deserialize(buf);
                if (!ack.has_value()) {
                    std::cerr << "[chunk/ack] malformed ChunkAck size=" << cb << " conn=" << incoming << "\n";
                    pMsg->Release();
                    continue;
                }

                if (ack->ackedType == static_cast<uint8_t>(PacketType::ChunkData)) {
                    const ChunkCoord coord{ ack->chunkX, ack->chunkY, ack->chunkZ };
                    std::lock_guard<std::mutex> lk(m_mutex);
                    auto it = m_clients.find(incoming);
                    if (it != m_clients.end()) {
                        auto pendingIt = it->second.pendingChunkData.find(coord);
                        auto expectedIt = it->second.pendingChunkDataPayloadHash.find(coord);
                        const bool wasPending = (pendingIt != it->second.pendingChunkData.end());
                        const bool wasStreamedAlready = it->second.streamedChunks.find(coord) != it->second.streamedChunks.end();
                        const bool hadExpectedHash = (expectedIt != it->second.pendingChunkDataPayloadHash.end());
                        const uint32_t expectedPayloadHash = hadExpectedHash ? expectedIt->second : 0;
                        const bool hashMatches = !hadExpectedHash || (ack->sequence == expectedPayloadHash);

                        if (wasPending && hashMatches) {
                            it->second.pendingChunkData.erase(pendingIt);
                            if (hadExpectedHash) {
                                it->second.pendingChunkDataPayloadHash.erase(expectedIt);
                            }
                            it->second.streamedChunks.insert(coord);
                        }
                        else if (wasPending && !hashMatches) {
                            // Keep pending so the chunk is retried; bypass retry cooldown for quick resend.
                            pendingIt->second = std::chrono::steady_clock::time_point::min();
                            std::cerr
                                << "[chunk/ack] payload hash mismatch conn=" << incoming
                                << " chunk=(" << coord.x << "," << coord.y << "," << coord.z << ")"
                                << " expected=" << expectedPayloadHash
                                << " got=" << ack->sequence
                                << " version=" << ack->version << "\n";
                        }
                        else if (!wasPending && !wasStreamedAlready) {
                            std::cerr
                                << "[chunk/ack] unexpected ChunkData ACK conn=" << incoming
                                << " chunk=(" << coord.x << "," << coord.y << "," << coord.z << ")"
                                << " seq=" << ack->sequence
                                << " version=" << ack->version << "\n";
                        }
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
                PlayerID playerId = 0;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    auto it = m_clients.find(incoming);
                    if (it != m_clients.end()) {
                        username = it->second.username;
                        playerId = it->second.playerId;
                    }
                }

                if (username.empty()) {
                    std::cout << "[recv] ShootRequest from unregistered conn = " << incoming << "\n";
                    pMsg->Release();
                    continue;
                }
                if (playerId != 0) {
                    m_playerManager.touchHeartbeat(playerId);
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
        std::vector<std::pair<HSteamNetConnection, ClientSession>> staleConnections;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_clients.begin(); it != m_clients.end();) {
                HSteamNetConnection conn = it->first;
                SteamNetConnectionInfo_t info;
                if (SteamNetworkingSockets()->GetConnectionInfo(conn, &info) &&
                    (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)) {
                    staleConnections.emplace_back(conn, it->second);
                    it = m_clients.erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (const auto& [conn, session] : staleConnections) {
            std::cout << "[cleanup] remove conn=" << conn << " user=" << session.username << "\n";
            ClearChunkPipelineForConnection(conn);
            if (session.playerId != 0) {
                m_playerManager.removePlayer(session.playerId);
            }
            if (!session.username.empty()) {
                std::string out;
                out.push_back(static_cast<char>(PacketType::ClientDisconnect));
                out += session.username;
                BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), conn);
            }
            SteamNetworkingSockets()->CloseConnection(conn, 0, "server cleanup", false);
        }

        const auto snapshotNow = std::chrono::steady_clock::now();
        if (snapshotNow - lastSnapshotTime >= snapshotInterval) {
            lastSnapshotTime = snapshotNow;

            std::vector<std::pair<HSteamNetConnection, PlayerID>> recipients;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                recipients.reserve(m_clients.size());
                for (const auto& [conn, session] : m_clients) {
                    if (session.playerId != 0) {
                        recipients.emplace_back(conn, session.playerId);
                    }
                }
            }

            std::vector<HSteamNetConnection> staleRecipients;
            for (const auto& [conn, playerId] : recipients) {
                std::vector<uint8_t> snapshot = m_playerManager.buildSnapshotFor(playerId);
                if (snapshot.empty()) {
                    staleRecipients.push_back(conn);
                    continue;
                }

                std::vector<uint8_t> packet;
                packet.reserve(1 + snapshot.size());
                packet.push_back(static_cast<uint8_t>(PacketType::PlayerSnapshot));
                packet.insert(packet.end(), snapshot.begin(), snapshot.end());
                SteamNetworkingSockets()->SendMessageToConnection(
                    conn,
                    packet.data(),
                    static_cast<uint32_t>(packet.size()),
                    k_nSteamNetworkingSend_UnreliableNoDelay,
                    nullptr
                );
            }

            if (!staleRecipients.empty()) {
                std::vector<std::pair<HSteamNetConnection, ClientSession>> removedSessions;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    for (HSteamNetConnection conn : staleRecipients) {
                        auto it = m_clients.find(conn);
                    if (it != m_clients.end()) {
                        removedSessions.emplace_back(it->first, it->second);
                            m_clients.erase(it);
                        }
                    }
                }

                for (const auto& [conn, session] : removedSessions) {
                    ClearChunkPipelineForConnection(conn);
                    if (!session.username.empty()) {
                        std::string out;
                        out.push_back(static_cast<char>(PacketType::ClientDisconnect));
                        out += session.username;
                        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), conn);
                    }
                    SteamNetworkingSockets()->CloseConnection(conn, 0, "server player timeout", false);
                }
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


