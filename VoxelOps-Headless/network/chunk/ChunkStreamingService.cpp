#include "ChunkStreamingService.hpp"

#include "../../engine/voxels/ServerChunk.hpp"
#include "../core/LockWaitTelemetry.hpp"
#include "../core/DiagnosticsFlags.hpp"
#include "../protocol/PacketParsers.hpp"
#include "../replication/CompressChunk.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace {

int FloorDiv(int a, int b) {
    int q = a / b;
    const int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

} // namespace

ChunkStreamingService::ChunkStreamingService(
    std::mutex &sessionMutex,
    ClientSessionManager &sessions,
    ChunkManager &chunkManager,
    ChunkPipelineState &pipelineState
)
    : m_sessionMutex(sessionMutex)
    , m_sessions(sessions)
    , m_chunkManager(chunkManager)
    , m_pipelineState(pipelineState) {}

ChunkStreamingService::~ChunkStreamingService() {
    StopChunkPipeline();
}

uint16_t ChunkStreamingService::ClampViewDistance(uint16_t requested) {
    constexpr uint16_t kMin = 2;
    const int spanX = WORLD_MAX_X - WORLD_MIN_X;
    const int spanZ = WORLD_MAX_Z - WORLD_MIN_Z;
    const int diagonalRadius =
        static_cast<int>(std::ceil(std::sqrt(static_cast<double>(spanX * spanX + spanZ * spanZ))));
    const uint16_t kMax = static_cast<uint16_t>(std::max(static_cast<int>(kMin), diagonalRadius));
    return std::clamp(requested, kMin, kMax);
}

std::string ChunkStreamingService::AllocateAutoUsernameLocked(HSteamNetConnection incomingConn) {
    constexpr uint32_t kNameSpaceSize = 10000;
    for (uint32_t attempt = 0; attempt < kNameSpaceSize; ++attempt) {
        const uint32_t suffix = (m_nextAutoUsername + attempt) % kNameSpaceSize;
        std::ostringstream oss;
        oss << "player#" << std::setfill('0') << std::setw(4) << suffix;
        const std::string candidate = oss.str();

        bool taken = false;
        for (const auto &[conn, session] : m_sessions) {
            if (conn == incomingConn || session.username.empty()) {
                continue;
            }
            if (session.username == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            taken = m_sessions.IsUsernamePendingForOtherConnection(incomingConn, candidate);
        }

        if (!taken) {
            m_nextAutoUsername = (suffix + 1) % kNameSpaceSize;
            return candidate;
        }
    }

    return {};
}

std::string ChunkStreamingService::BuildDisplayNameForIdentityLocked(
    std::string_view identity, std::string_view requestedName, HSteamNetConnection incomingConn
) {
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

    auto nameTaken = [&](const std::string &candidate) {
        for (const auto &[conn, session] : m_sessions) {
            if (conn == incomingConn || session.username.empty()) {
                continue;
            }
            if (session.username == candidate) {
                return true;
            }
        }
        if (m_sessions.IsUsernamePendingForOtherConnection(incomingConn, candidate)) {
            return true;
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

void ChunkStreamingService::StartChunkPipeline() {
    m_pipelineState.Clear();
    m_chunkPrepQuit.store(false, std::memory_order_release);
    if (!m_chunkPrepThread.joinable()) {
        m_chunkPrepThread = std::thread([this]() { ChunkPrepWorkerLoop(); });
    }
}

void ChunkStreamingService::StopChunkPipeline() {
    m_chunkPrepQuit.store(true, std::memory_order_release);
    m_pipelineState.NotifyPrepWorkerAll();
    if (m_chunkPrepThread.joinable()) {
        m_chunkPrepThread.join();
    }
    m_pipelineState.Clear();
    m_chunkPrepQuit.store(false, std::memory_order_release);
}

bool ChunkStreamingService::PrepareChunkForStreaming(const ChunkCoord &coord) {
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

    ServerChunk *chunk = m_chunkManager.loadOrGenerateChunk(glm::ivec3(coord.x, coord.y, coord.z));
    return chunk != nullptr;
}

void ChunkStreamingService::ChunkPrepWorkerLoop() {
    while (true) {
        ChunkPipelineState::ChunkPrepTask task;
        if (!m_pipelineState.WaitPopPrepTask(task, m_chunkPrepQuit)) {
            return;
        }

        bool stillNeeded = false;
        {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_sessionMutex, "ChunkStreamingService::ChunkPrepWorkerLoop"
            );
            auto it = m_sessions.find(task.conn);
            if (it != m_sessions.end()) {
                stillNeeded = it->second.pendingChunkData.find(task.coord) !=
                              it->second.pendingChunkData.end();
            }
        }

        const bool prepared = stillNeeded && PrepareChunkForStreaming(task.coord);
        m_pipelineState.MarkPrepDoneAndQueueSend(
            task, prepared, m_chunkPrepQuit, kMaxChunkSendQueuePerClient
        );
    }
}

bool ChunkStreamingService::QueueChunkPreparation(HSteamNetConnection conn, const ChunkCoord &coord) {
    const auto result = m_pipelineState.QueuePrep(conn, coord, kMaxChunkPrepQueue);
    if (result == ChunkPipelineState::QueuePrepResult::Queued) {
        m_pipelineState.NotifyPrepWorker();
        return true;
    }
    if (result == ChunkPipelineState::QueuePrepResult::AlreadyQueued) {
        return true;
    }
    return false;
}

size_t ChunkStreamingService::FlushChunkSendQueueForClient(HSteamNetConnection conn, size_t maxSends) {
    std::unordered_set<ChunkCoord, ChunkCoordHash> pendingCoords;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::FlushChunkSendQueueForClient.snapshotPending"
        );
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end()) {
            pendingCoords.reserve(it->second.pendingChunkData.size());
            for (const auto &[coord, _] : it->second.pendingChunkData) {
                pendingCoords.insert(coord);
            }
        }
    }

    size_t sent = 0;
    std::vector<ChunkCoord> deliveredCoords;
    deliveredCoords.reserve(maxSends);
    while (sent < maxSends) {
        ChunkCoord coord{};
        if (!m_pipelineState.PopNextSendChunk(conn, coord)) {
            break;
        }

        if (pendingCoords.find(coord) == pendingCoords.end()) {
            continue;
        }

        if (!SendChunkData(conn, coord)) {
            continue;
        }

        pendingCoords.erase(coord);
        deliveredCoords.push_back(coord);
        ++sent;
    }

    if (!deliveredCoords.empty()) {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::FlushChunkSendQueueForClient.applyDelivered"
        );
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end()) {
            for (const ChunkCoord &coord : deliveredCoords) {
                it->second.pendingChunkData.erase(coord);
                it->second.streamedChunks.insert(coord);
            }
        }
    }
    return sent;
}

size_t ChunkStreamingService::FlushChunkSendQueues(size_t globalBudget, size_t perClientBudget) {
    if (globalBudget == 0 || perClientBudget == 0) {
        return 0;
    }

    std::vector<HSteamNetConnection> clients;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::FlushChunkSendQueues.snapshotClients"
        );
        clients.reserve(m_sessions.size());
        for (const auto &kv : m_sessions) {
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

void ChunkStreamingService::PruneChunkPipelineForClient(
    HSteamNetConnection conn, const std::unordered_set<ChunkCoord, ChunkCoordHash> &desired
) {
    m_pipelineState.PruneForClient(conn, desired);
}

size_t ChunkStreamingService::GetChunkSendQueueDepthForClient(HSteamNetConnection conn) {
    return m_pipelineState.GetSendQueueDepthForClient(conn);
}

void ChunkStreamingService::ClearChunkPipelineForConnection(HSteamNetConnection conn) {
    m_pipelineState.ClearForConnection(conn);
}

bool ChunkStreamingService::SendChunkData(HSteamNetConnection conn, const ChunkCoord &coord) {
    ServerChunk *chunk = m_chunkManager.getChunkIfExists(glm::ivec3(coord.x, coord.y, coord.z));
    if (!chunk) {
        std::cerr << "[chunk/send] chunk missing after prep for conn=" << conn << " chunk=("
                  << coord.x << "," << coord.y << "," << coord.z << ")\n";
        return false;
    }

    ChunkData packet;
    packet.chunkX = coord.x;
    packet.chunkY = coord.y;
    packet.chunkZ = coord.z;
    packet.version = static_cast<uint64_t>(std::max<int64_t>(0, chunk->version()));
    std::vector<uint8_t> rawPayload(CHUNK_VOLUME * sizeof(BlockID));
    chunk->fillRawVoxelBytes(rawPayload.data(), rawPayload.size());
    const CompressedChunkPayload compressedPayload = CompressChunkPayload(rawPayload);
    packet.flags = compressedPayload.compressed ? 0x1u : 0u;
    packet.payload = compressedPayload.payload;

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
        std::cerr << "[chunk/send] SendMessageToConnection failed result=" << result
                  << " conn=" << conn << " chunk=(" << coord.x << "," << coord.y << "," << coord.z
                  << ")"
                  << " bytes=" << bytes.size();
        if (haveInfo) {
            std::cerr << " connState=" << info.m_eState;
        }
        std::cerr << "\n";
    }
    return result == k_EResultOK;
}

bool ChunkStreamingService::SendChunkUnload(HSteamNetConnection conn, const ChunkCoord &coord) {
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

void ChunkStreamingService::UpdateChunkStreamingForClient(
    HSteamNetConnection conn, const glm::ivec3 &centerChunk, uint16_t viewDistance
) {
    constexpr size_t kMaxChunkPrepQueuePerUpdate = 128;
    constexpr size_t kMaxPendingChunkData = 256;
    constexpr size_t kMaxChunkUnloadsPerUpdate = 24;
    constexpr auto kChunkRetryInterval = std::chrono::milliseconds(500);
    const uint16_t clampedViewDistance = ClampViewDistance(viewDistance);
    const auto now = std::chrono::steady_clock::now();

    std::unordered_set<ChunkCoord, ChunkCoordHash> desired;
    const int minChunkY = FloorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = FloorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    const int radius = static_cast<int>(clampedViewDistance);
    const int64_t radius2 = static_cast<int64_t>(radius) * static_cast<int64_t>(radius);
    desired.reserve(
        static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1) * (maxChunkY - minChunkY + 1))
    );

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
                desired.insert(ChunkCoord{x, y, z});
            }
        }
    }

    std::unordered_set<ChunkCoord, ChunkCoordHash> toUnloadSet;
    std::unordered_set<ChunkCoord, ChunkCoordHash> retryToLoad;
    std::vector<ChunkCoord> toLoad;
    size_t pendingCount = 0;
    size_t streamedCount = 0;
    bool hadStreamedChunks = false;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.snapshot"
        );
        auto it = m_sessions.find(conn);
        if (it == m_sessions.end()) {
            return;
        }

        it->second.interestCenterChunk = centerChunk;
        it->second.viewDistance = clampedViewDistance;
        it->second.hasChunkInterest = true;

        for (auto pIt = it->second.pendingChunkData.begin(); pIt != it->second.pendingChunkData.end();
             ) {
            if (desired.find(pIt->first) == desired.end()) {
                toUnloadSet.insert(pIt->first);
                pIt = it->second.pendingChunkData.erase(pIt);
            } else {
                ++pIt;
            }
        }
        streamedCount = it->second.streamedChunks.size();
        hadStreamedChunks = streamedCount > 0;
        pendingCount = it->second.pendingChunkData.size();
        toLoad.reserve(desired.size());
        retryToLoad.reserve(it->second.pendingChunkData.size());
        for (const ChunkCoord &c : desired) {
            if (it->second.streamedChunks.find(c) != it->second.streamedChunks.end()) {
                continue;
            }

            const auto pendingIt = it->second.pendingChunkData.find(c);
            if (pendingIt != it->second.pendingChunkData.end() &&
                (now - pendingIt->second) < kChunkRetryInterval) {
                continue;
            }

            if (pendingIt != it->second.pendingChunkData.end()) {
                retryToLoad.insert(c);
            }
            toLoad.push_back(c);
        }

        for (const ChunkCoord &c : it->second.streamedChunks) {
            if (desired.find(c) == desired.end()) {
                toUnloadSet.insert(c);
            }
        }
    }

    PruneChunkPipelineForClient(conn, desired);

    const bool isInitialSync = !hadStreamedChunks;
    int verticalAnchorY = std::clamp(centerChunk.y, minChunkY, maxChunkY);
    if (verticalAnchorY == maxChunkY && maxChunkY > minChunkY) {
        --verticalAnchorY;
    }
    std::sort(toLoad.begin(), toLoad.end(), [&](const ChunkCoord &a, const ChunkCoord &b) {
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

        if (a.x != b.x)
            return a.x < b.x;
        if (a.y != b.y)
            return a.y < b.y;
        return a.z < b.z;
    });

    std::vector<ChunkCoord> toUnload;
    toUnload.reserve(toUnloadSet.size());
    for (const ChunkCoord &c : toUnloadSet) {
        toUnload.push_back(c);
    }

    size_t queuedPrepThisUpdate = 0;
    bool stoppedByPendingCap = false;
    bool stoppedByPrepCap = false;
    std::vector<ChunkCoord> queuedPrepCoords;
    queuedPrepCoords.reserve(std::min<size_t>(toLoad.size(), kMaxChunkPrepQueuePerUpdate));
    for (const ChunkCoord &c : toLoad) {
        const bool isRetry = retryToLoad.find(c) != retryToLoad.end();
        if (queuedPrepThisUpdate >= kMaxChunkPrepQueuePerUpdate) {
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

        queuedPrepCoords.push_back(c);
        if (!isRetry) {
            ++pendingCount;
        }
        ++queuedPrepThisUpdate;
    }

    if (!queuedPrepCoords.empty()) {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.applyPending"
        );
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end()) {
            pendingCount = it->second.pendingChunkData.size();
            for (const ChunkCoord &c : queuedPrepCoords) {
                const bool wasPending =
                    it->second.pendingChunkData.find(c) != it->second.pendingChunkData.end();
                it->second.pendingChunkData[c] = now;
                if (!wasPending) {
                    ++pendingCount;
                }
            }
        }
    }

    const size_t sendQueueDepth = GetChunkSendQueueDepthForClient(conn);

    static std::unordered_map<HSteamNetConnection, std::chrono::steady_clock::time_point>
        s_lastProgressLog;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.logCleanup"
        );
        for (auto it = s_lastProgressLog.begin(); it != s_lastProgressLog.end();) {
            if (m_sessions.find(it->first) == m_sessions.end()) {
                it = s_lastProgressLog.erase(it);
            } else {
                ++it;
            }
        }
    }
    auto &lastLog = s_lastProgressLog[conn];
    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        (now - lastLog) >= std::chrono::seconds(1)) {
        lastLog = now;
        std::cerr << "[chunk/stream] progress conn=" << conn << " desired=" << desired.size()
                  << " streamed=" << streamedCount << " pending=" << pendingCount
                  << " toLoad=" << toLoad.size() << " queuedPrepNow=" << queuedPrepThisUpdate
                  << " pendingCapHit=" << (stoppedByPendingCap ? 1 : 0)
                  << " prepCapHit=" << (stoppedByPrepCap ? 1 : 0) << " sendQueue=" << sendQueueDepth
                  << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z
                  << ")"
                  << " viewDist=" << clampedViewDistance << "\n";
    }

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        !toLoad.empty() && queuedPrepThisUpdate == 0 && !stoppedByPendingCap &&
        !stoppedByPrepCap && sendQueueDepth == 0) {
        std::cerr << "[chunk/stream] stalled load window conn=" << conn
                  << " desired=" << desired.size() << " toLoad=" << toLoad.size()
                  << " streamed=" << streamedCount << " pending=" << pendingCount
                  << " pendingCap=" << kMaxPendingChunkData
                  << " prepQueueCap=" << kMaxChunkPrepQueue << " sendQueue=" << sendQueueDepth
                  << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z
                  << ")"
                  << " viewDist=" << clampedViewDistance << "\n";
    }

    size_t unloadsSentThisUpdate = 0;
    std::vector<ChunkCoord> successfullyUnloaded;
    successfullyUnloaded.reserve(std::min<size_t>(toUnload.size(), kMaxChunkUnloadsPerUpdate));
    for (const ChunkCoord &c : toUnload) {
        if (unloadsSentThisUpdate >= kMaxChunkUnloadsPerUpdate) {
            break;
        }
        if (!SendChunkUnload(conn, c)) {
            continue;
        }

        successfullyUnloaded.push_back(c);
        ++unloadsSentThisUpdate;
    }

    if (!successfullyUnloaded.empty()) {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.applyUnload"
        );
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end()) {
            for (const ChunkCoord &c : successfullyUnloaded) {
                it->second.streamedChunks.erase(c);
                it->second.pendingChunkData.erase(c);
            }
        } else {
            unloadsSentThisUpdate = 0;
        }
    }

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        toUnload.size() > unloadsSentThisUpdate && unloadsSentThisUpdate > 0) {
        std::cerr << "[chunk/stream] unload throttle conn=" << conn
                  << " requested=" << toUnload.size() << " sentNow=" << unloadsSentThisUpdate
                  << " deferred=" << (toUnload.size() - unloadsSentThisUpdate)
                  << " cap=" << kMaxChunkUnloadsPerUpdate << "\n";
    }
}
