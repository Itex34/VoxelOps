#include "ServerNetwork.hpp"
#include "CompressChunk.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

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

std::string ServerNetwork::BuildDisplayNameForIdentityLocked(
    std::string_view identity,
    std::string_view requestedName,
    HSteamNetConnection incomingConn
)
{
    std::string base;
    if (!requestedName.empty()) {
        base.assign(requestedName.begin(), requestedName.end());
    }
    if (base.empty()) {
        base = "player-";
        base.append(identity.substr(0, std::min<size_t>(identity.size(), 8)));
    }
    if (base.size() > kMaxConnectUsernameChars) {
        base.resize(kMaxConnectUsernameChars);
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    auto nameTaken = [&](const std::string& candidate) {
        for (const auto& [conn, session] : m_clients) {
            if (conn == incomingConn || session.username.empty()) {
                continue;
            }
            if (session.username == candidate) {
                return true;
            }
        }
        return false;
    };

    if (!nameTaken(base)) {
        return base;
    }

    for (uint32_t suffix = 2; suffix < 10000; ++suffix) {
        const std::string suffixText = "#" + std::to_string(suffix);
        std::string candidate = base;
        if (candidate.size() + suffixText.size() > kMaxConnectUsernameChars) {
            candidate.resize(kMaxConnectUsernameChars - suffixText.size());
        }
        candidate += suffixText;
        if (!nameTaken(candidate)) {
            return candidate;
        }
    }

    return AllocateAutoUsernameLocked(incomingConn);
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

size_t ServerNetwork::FlushChunkSendQueues(size_t globalBudget, size_t perClientBudget)
{
    if (globalBudget == 0 || perClientBudget == 0) {
        return 0;
    }

    std::vector<HSteamNetConnection> clients;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        clients.reserve(m_clients.size());
        for (const auto& kv : m_clients) {
            clients.push_back(kv.first);
        }
    }

    size_t totalSent = 0;
    for (HSteamNetConnection conn : clients) {
        if (totalSent >= globalBudget) {
            break;
        }
        const size_t remaining = globalBudget - totalSent;
        const size_t perClientCap = std::min(perClientBudget, remaining);
        totalSent += FlushChunkSendQueueForClient(conn, perClientCap);
    }

    return totalSent;
}

void ServerNetwork::PruneChunkPipelineForClient(
    HSteamNetConnection conn,
    const std::unordered_set<ChunkCoord, ChunkCoordHash>& desired
)
{
    std::lock_guard<std::mutex> lk(m_chunkPipelineMutex);

    for (auto it = m_chunkPrepQueue.begin(); it != m_chunkPrepQueue.end();) {
        if (it->conn == conn && desired.find(it->coord) == desired.end()) {
            m_chunkPrepQueued.erase(ChunkPipelineKey{ conn, it->coord });
            it = m_chunkPrepQueue.erase(it);
        }
        else {
            ++it;
        }
    }

    auto sendIt = m_chunkSendQueues.find(conn);
    if (sendIt != m_chunkSendQueues.end()) {
        auto& sendQueue = sendIt->second;
        for (auto it = sendQueue.begin(); it != sendQueue.end();) {
            if (desired.find(*it) == desired.end()) {
                m_chunkSendQueued.erase(ChunkPipelineKey{ conn, *it });
                it = sendQueue.erase(it);
            }
            else {
                ++it;
            }
        }
        if (sendQueue.empty()) {
            m_chunkSendQueues.erase(sendIt);
        }
    }
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

