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
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

    int FloorDiv(int a, int b) {
        int q = a / b;
        const int r = a % b;
        if ((r != 0) && ((r > 0) != (b > 0))) {
            --q;
        }
        return q;
    }

    struct OrderedChunkOffset {
        int dx = 0;
        int y = 0;
        int dz = 0;
    };

    bool IsDesiredChunk(
        const ClientSessionManager::ChunkCoord &coord,
        const ChunkPipelineState::ChunkInterestBounds &bounds
    ) noexcept {
        return bounds.contains(coord);
    }

    const std::vector<OrderedChunkOffset> &
    GetOrderedChunkOffsets(int radius, int verticalAnchorY, int minChunkY, int maxChunkY) {
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint16_t>(radius)) << 48) |
                             (static_cast<uint64_t>(static_cast<uint16_t>(verticalAnchorY)) << 32) |
                             (static_cast<uint64_t>(static_cast<uint16_t>(minChunkY)) << 16) |
                             static_cast<uint64_t>(static_cast<uint16_t>(maxChunkY));
        static std::unordered_map<uint64_t, std::vector<OrderedChunkOffset>> s_cache;
        auto existing = s_cache.find(key);
        if (existing != s_cache.end()) {
            return existing->second;
        }

        std::vector<OrderedChunkOffset> offsets;
        const int64_t radius2 = static_cast<int64_t>(radius) * static_cast<int64_t>(radius);
        offsets.reserve(
            static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1) * (maxChunkY - minChunkY + 1))
        );
        for (int dx = -radius; dx <= radius; ++dx) {
            const int64_t dx2 = static_cast<int64_t>(dx) * static_cast<int64_t>(dx);
            for (int dz = -radius; dz <= radius; ++dz) {
                const int64_t dz2 = static_cast<int64_t>(dz) * static_cast<int64_t>(dz);
                if (dx2 + dz2 > radius2) {
                    continue;
                }
                for (int y = minChunkY; y <= maxChunkY; ++y) {
                    offsets.push_back(OrderedChunkOffset{dx, y, dz});
                }
            }
        }

        std::sort(
            offsets.begin(),
            offsets.end(),
            [&](const OrderedChunkOffset &a, const OrderedChunkOffset &b) {
                const int aHorizDist2 = a.dx * a.dx + a.dz * a.dz;
                const int bHorizDist2 = b.dx * b.dx + b.dz * b.dz;
                if (aHorizDist2 != bHorizDist2) {
                    return aHorizDist2 < bHorizDist2;
                }

                const bool aUnderOrSame = (a.y <= verticalAnchorY);
                const bool bUnderOrSame = (b.y <= verticalAnchorY);
                if (aUnderOrSame != bUnderOrSame) {
                    return aUnderOrSame;
                }

                const int aVert = std::abs(a.y - verticalAnchorY);
                const int bVert = std::abs(b.y - verticalAnchorY);
                if (aVert != bVert) {
                    return aVert < bVert;
                }

                if (a.dx != b.dx) {
                    return a.dx < b.dx;
                }
                if (a.y != b.y) {
                    return a.y < b.y;
                }
                return a.dz < b.dz;
            }
        );

        auto [inserted, _] = s_cache.emplace(key, std::move(offsets));
        return inserted->second;
    }

    size_t ChunkPrepWorkerCount() {
        constexpr size_t kMaxChunkPrepWorkers = 6;
        const unsigned int hardwareThreads = std::thread::hardware_concurrency();
        if (hardwareThreads == 0) {
            return 2;
        }
        const size_t spareWorkerThreads =
            hardwareThreads > 2 ? static_cast<size_t>(hardwareThreads - 2) : 1u;
        return std::clamp<size_t>(spareWorkerThreads, 2u, kMaxChunkPrepWorkers);
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
    if (m_chunkPrepThreads.empty()) {
        const size_t workerCount = ChunkPrepWorkerCount();
        m_chunkPrepThreads.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            m_chunkPrepThreads.emplace_back([this]() { ChunkPrepWorkerLoop(); });
        }
    }
}

void ChunkStreamingService::StopChunkPipeline() {
    m_chunkPrepQuit.store(true, std::memory_order_release);
    m_pipelineState.NotifyPrepWorkerAll();
    m_chunkPrepareCv.notify_all();
    for (std::thread &worker : m_chunkPrepThreads) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_chunkPrepThreads.clear();
    {
        std::lock_guard<std::mutex> lk(m_chunkPrepareMutex);
        m_chunksBeingPrepared.clear();
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

        bool prepared = false;
        if (stillNeeded) {
            bool ownsChunkPrep = false;
            {
                std::unique_lock<std::mutex> lk(m_chunkPrepareMutex);
                m_chunkPrepareCv.wait(lk, [&]() {
                    return m_chunkPrepQuit.load(std::memory_order_acquire) ||
                           m_chunksBeingPrepared.find(task.coord) == m_chunksBeingPrepared.end();
                });
                if (!m_chunkPrepQuit.load(std::memory_order_acquire)) {
                    if (m_chunkManager.isChunkStreamReady(
                            glm::ivec3(task.coord.x, task.coord.y, task.coord.z)
                        )) {
                        prepared = true;
                    } else {
                        m_chunksBeingPrepared.insert(task.coord);
                        ownsChunkPrep = true;
                    }
                }
            }

            if (ownsChunkPrep) {
                prepared = PrepareChunkForStreaming(task.coord);
                {
                    std::lock_guard<std::mutex> lk(m_chunkPrepareMutex);
                    m_chunksBeingPrepared.erase(task.coord);
                }
                m_chunkPrepareCv.notify_all();
            }
        }
        m_pipelineState.MarkPrepDoneAndQueueSend(
            task, prepared, m_chunkPrepQuit, kMaxChunkSendQueuePerClient
        );
    }
}

bool ChunkStreamingService::QueueChunkPreparation(
    HSteamNetConnection conn, const ChunkCoord &coord
) {
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

ChunkPipelineState::QueuePrepBatchResult ChunkStreamingService::QueueChunkPreparations(
    HSteamNetConnection conn,
    const std::vector<ChunkCoord> &coords,
    std::vector<ChunkCoord> &acceptedCoords
) {
    std::vector<ChunkCoord> readyCoords;
    std::vector<ChunkCoord> prepCoords;
    readyCoords.reserve(coords.size());
    prepCoords.reserve(coords.size());
    for (const ChunkCoord &coord : coords) {
        if (m_chunkManager.isChunkStreamReady(glm::ivec3(coord.x, coord.y, coord.z))) {
            readyCoords.push_back(coord);
        } else {
            prepCoords.push_back(coord);
        }
    }

    ChunkPipelineState::QueuePrepBatchResult result{};
    if (!readyCoords.empty()) {
        const auto sendResult = m_pipelineState.QueuePreparedSendBatch(
            conn, readyCoords, kMaxChunkSendQueuePerClient, acceptedCoords
        );
        result.accepted += sendResult.accepted;
        result.queueFull = result.queueFull || sendResult.queueFull;
    }
    if (!prepCoords.empty()) {
        const auto prepResult =
            m_pipelineState.QueuePrepBatch(conn, prepCoords, kMaxChunkPrepQueue, acceptedCoords);
        result.accepted += prepResult.accepted;
        result.queueFull = result.queueFull || prepResult.queueFull;
        if (prepResult.accepted > 0) {
            m_pipelineState.NotifyPrepWorkerAll();
        }
    }
    return result;
}

bool ChunkStreamingService::IsConnectionSendable(HSteamNetConnection conn) const {
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info)) {
        return false;
    }
    return info.m_eState == k_ESteamNetworkingConnectionState_Connected;
}

void ChunkStreamingService::ClearClientChunkState(HSteamNetConnection conn) {
    m_pipelineState.ClearForConnection(conn);
    auto lk = LockWaitTelemetry::AcquireSessionLock(
        m_sessionMutex, "ChunkStreamingService::ClearClientChunkState"
    );
    auto it = m_sessions.find(conn);
    if (it == m_sessions.end()) {
        return;
    }
    it->second.pendingChunkData.clear();
    it->second.streamedChunks.clear();
    it->second.hasChunkInterest = false;
    it->second.chunkInterestDirty = false;
}

size_t
ChunkStreamingService::FlushChunkSendQueueForClient(HSteamNetConnection conn, size_t maxSends) {
    if (!IsConnectionSendable(conn)) {
        ClearClientChunkState(conn);
        return 0;
    }

    std::vector<ChunkCoord> poppedCoords;
    poppedCoords.reserve(maxSends);
    m_pipelineState.PopNextSendChunks(conn, maxSends, poppedCoords);
    if (poppedCoords.empty()) {
        return 0;
    }

    std::vector<ChunkCoord> sendCoords;
    sendCoords.reserve(poppedCoords.size());
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::FlushChunkSendQueueForClient.snapshotPending"
        );
        auto it = m_sessions.find(conn);
        if (it == m_sessions.end()) {
            return 0;
        }
        for (const ChunkCoord &coord : poppedCoords) {
            if (it->second.pendingChunkData.find(coord) != it->second.pendingChunkData.end()) {
                sendCoords.push_back(coord);
            }
        }
    }
    if (sendCoords.empty()) {
        return 0;
    }

    size_t sent = 0;
    std::vector<ChunkCoord> deliveredCoords;
    deliveredCoords.reserve(sendCoords.size());
    for (const ChunkCoord &coord : sendCoords) {
        if (!IsConnectionSendable(conn)) {
            ClearClientChunkState(conn);
            break;
        }

        if (!SendChunkData(conn, coord)) {
            ClearClientChunkState(conn);
            break;
        }

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
    const size_t startIndex = clients.empty() ? 0 : (m_nextChunkSendClientIndex % clients.size());
    size_t visitedClients = 0;
    for (; visitedClients < clients.size(); ++visitedClients) {
        if (totalSent >= globalBudget) {
            break;
        }
        const size_t clientIndex = (startIndex + visitedClients) % clients.size();
        HSteamNetConnection conn = clients[clientIndex];
        const size_t remaining = globalBudget - totalSent;
        const size_t perClientCap = std::min(perClientBudget, remaining);
        totalSent += FlushChunkSendQueueForClient(conn, perClientCap);
    }
    if (!clients.empty()) {
        m_nextChunkSendClientIndex =
            (startIndex + std::max<size_t>(visitedClients, 1)) % clients.size();
    } else {
        m_nextChunkSendClientIndex = 0;
    }

    return totalSent;
}

void ChunkStreamingService::PruneChunkPipelineForClient(
    HSteamNetConnection conn, const ChunkPipelineState::ChunkInterestBounds &desired
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

    const uint64_t version = static_cast<uint64_t>(std::max<int64_t>(0, chunk->version()));
    auto cacheIt = m_chunkPacketCache.find(coord);
    if (cacheIt == m_chunkPacketCache.end() || cacheIt->second.version != version) {
        ChunkData packet;
        packet.chunkX = coord.x;
        packet.chunkY = coord.y;
        packet.chunkZ = coord.z;
        packet.version = version;
        std::vector<uint8_t> rawPayload(CHUNK_VOLUME * sizeof(BlockID));
        chunk->fillRawVoxelBytes(rawPayload.data(), rawPayload.size());
        const CompressedChunkPayload compressedPayload = CompressChunkPayload(rawPayload);
        packet.flags = compressedPayload.compressed ? 0x1u : 0u;
        packet.payload = compressedPayload.payload;

        CachedChunkPacket cached;
        cached.version = version;
        cached.bytes = packet.serialize();
        cacheIt = m_chunkPacketCache.insert_or_assign(coord, std::move(cached)).first;
    }

    const std::vector<uint8_t> &bytes = cacheIt->second.bytes;
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
        std::cerr << " clearingChunkState=1\n";
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
    constexpr size_t kMaxPendingChunkData = 512;
    constexpr size_t kMaxChunkUnloadsPerUpdate = 24;
    constexpr auto kChunkRetryInterval = std::chrono::milliseconds(500);
    const uint16_t clampedViewDistance = ClampViewDistance(viewDistance);
    const auto now = std::chrono::steady_clock::now();

    const int minChunkY = FloorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = FloorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    const int radius = static_cast<int>(clampedViewDistance);
    const ChunkWorldBounds &worldBounds = m_chunkManager.chunkWorldBounds();
    const ChunkPipelineState::ChunkInterestBounds desiredBounds{
        worldBounds.minChunk.x,
        worldBounds.maxChunk.x,
        minChunkY,
        maxChunkY,
        worldBounds.minChunk.z,
        worldBounds.maxChunk.z,
        centerChunk.x,
        centerChunk.z,
        radius,
    };
    int verticalAnchorY = std::clamp(centerChunk.y, minChunkY, maxChunkY);
    if (verticalAnchorY == maxChunkY && maxChunkY > minChunkY) {
        --verticalAnchorY;
    }
    const std::vector<OrderedChunkOffset> &orderedOffsets =
        GetOrderedChunkOffsets(radius, verticalAnchorY, minChunkY, maxChunkY);

    std::vector<ChunkCoord> desiredOrdered;
    desiredOrdered.reserve(orderedOffsets.size());
    for (const OrderedChunkOffset &offset : orderedOffsets) {
        ChunkCoord coord{centerChunk.x + offset.dx, offset.y, centerChunk.z + offset.dz};
        if (!IsDesiredChunk(coord, desiredBounds)) {
            continue;
        }
        desiredOrdered.push_back(coord);
    }

    std::unordered_set<ChunkCoord, ChunkCoordHash> toUnloadSet;
    std::vector<ChunkCoord> toLoad;
    bool interestChanged = false;
    bool hasMoreLoadWork = false;
    bool stoppedByPendingCap = false;
    size_t pendingCount = 0;
    size_t streamedCount = 0;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.snapshot"
        );
        auto it = m_sessions.find(conn);
        if (it == m_sessions.end()) {
            return;
        }

        interestChanged = !it->second.hasChunkInterest ||
                          it->second.interestCenterChunk.x != centerChunk.x ||
                          it->second.interestCenterChunk.y != centerChunk.y ||
                          it->second.interestCenterChunk.z != centerChunk.z ||
                          it->second.viewDistance != clampedViewDistance;
        it->second.interestCenterChunk = centerChunk;
        it->second.viewDistance = clampedViewDistance;
        it->second.hasChunkInterest = true;

        for (auto pIt = it->second.pendingChunkData.begin();
             pIt != it->second.pendingChunkData.end();) {
            if (!IsDesiredChunk(pIt->first, desiredBounds)) {
                toUnloadSet.insert(pIt->first);
                pIt = it->second.pendingChunkData.erase(pIt);
            } else {
                ++pIt;
            }
        }
        streamedCount = it->second.streamedChunks.size();
        pendingCount = it->second.pendingChunkData.size();
        toLoad.reserve(std::min<size_t>(desiredOrdered.size(), kMaxChunkPrepQueuePerUpdate));
        size_t projectedPendingCount = pendingCount;
        for (const ChunkCoord &c : desiredOrdered) {
            if (it->second.streamedChunks.find(c) != it->second.streamedChunks.end()) {
                continue;
            }

            const auto pendingIt = it->second.pendingChunkData.find(c);
            if (pendingIt != it->second.pendingChunkData.end() &&
                (now - pendingIt->second) < kChunkRetryInterval) {
                continue;
            }

            if (pendingIt == it->second.pendingChunkData.end() &&
                projectedPendingCount >= kMaxPendingChunkData) {
                stoppedByPendingCap = true;
                hasMoreLoadWork = true;
                break;
            }
            toLoad.push_back(c);
            if (pendingIt == it->second.pendingChunkData.end()) {
                ++projectedPendingCount;
            }
            if (toLoad.size() >= kMaxChunkPrepQueuePerUpdate) {
                hasMoreLoadWork = true;
                break;
            }
        }

        for (const ChunkCoord &c : it->second.streamedChunks) {
            if (!IsDesiredChunk(c, desiredBounds)) {
                toUnloadSet.insert(c);
            }
        }
    }

    if (interestChanged || !toUnloadSet.empty()) {
        PruneChunkPipelineForClient(conn, desiredBounds);
    }

    std::vector<ChunkCoord> toUnload;
    toUnload.reserve(toUnloadSet.size());
    for (const ChunkCoord &c : toUnloadSet) {
        toUnload.push_back(c);
    }

    size_t queuedPrepThisUpdate = 0;
    bool stoppedByPrepCap = false;
    std::vector<ChunkCoord> queuedPrepCoords;
    queuedPrepCoords.reserve(std::min<size_t>(toLoad.size(), kMaxChunkPrepQueuePerUpdate));

    if (!toLoad.empty()) {
        const auto prepResult = QueueChunkPreparations(conn, toLoad, queuedPrepCoords);
        queuedPrepThisUpdate = prepResult.accepted;
        stoppedByPrepCap = prepResult.queueFull;
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
    auto &lastLog = s_lastProgressLog[conn];

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        (now - lastLog) >= std::chrono::seconds(15)) {
        lastLog = now;
        std::cerr << "[chunk/stream] progress conn=" << conn << " desired=" << desiredOrdered.size()
                  << " streamed=" << streamedCount << " pending=" << pendingCount
                  << " toLoad=" << toLoad.size() << " queuedPrepNow=" << queuedPrepThisUpdate
                  << " pendingCapHit=" << (stoppedByPendingCap ? 1 : 0)
                  << " prepCapHit=" << (stoppedByPrepCap ? 1 : 0) << " sendQueue=" << sendQueueDepth
                  << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z
                  << ")"
                  << " viewDist=" << clampedViewDistance << "\n";
    }

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        !toLoad.empty() && queuedPrepThisUpdate == 0 && !stoppedByPendingCap && !stoppedByPrepCap &&
        sendQueueDepth == 0) {
        std::cerr << "[chunk/stream] stalled load window conn=" << conn
                  << " desired=" << desiredOrdered.size() << " toLoad=" << toLoad.size()
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

    const bool hasMoreInterestWork = hasMoreLoadWork || toLoad.size() > queuedPrepThisUpdate ||
                                     stoppedByPendingCap ||
                                     stoppedByPrepCap || toUnload.size() > unloadsSentThisUpdate;
    {
        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.reschedule"
        );
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end() && it->second.interestCenterChunk == centerChunk &&
            it->second.viewDistance == clampedViewDistance) {
            it->second.nextChunkInterestUpdateAt =
                hasMoreInterestWork ? now + std::chrono::milliseconds(100)
                                    : std::chrono::steady_clock::time_point::max();
        }
    }
}
