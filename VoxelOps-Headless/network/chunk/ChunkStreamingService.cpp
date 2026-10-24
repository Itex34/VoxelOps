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

    uint32_t Fnv1a32(const uint8_t *data, size_t size) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < size; ++i) {
            h ^= static_cast<uint32_t>(data[i]);
            h *= 16777619u;
        }
        return h;
    }

    struct RawChunkSummary {
        uint32_t hash = 0;
        uint16_t nonAir = 0;
        int minNonAirWorldY = 0;
        int maxNonAirWorldY = -1;
    };

    RawChunkSummary SummarizeRawChunkPayload(
        const std::vector<uint8_t> &rawPayload, const glm::ivec3 &chunkPos
    ) {
        RawChunkSummary summary{};
        summary.hash = Fnv1a32(rawPayload.data(), rawPayload.size());
        summary.minNonAirWorldY = chunkPos.y * CHUNK_SIZE;
        summary.maxNonAirWorldY = chunkPos.y * CHUNK_SIZE - 1;

        int minLocalY = CHUNK_SIZE;
        int maxLocalY = -1;
        size_t nonAir = 0;
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    const size_t i = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_SIZE * z));
                    if (i >= rawPayload.size()) {
                        continue;
                    }
                    if (static_cast<BlockID>(rawPayload[i]) != BlockID::Air) {
                        ++nonAir;
                        minLocalY = std::min(minLocalY, y);
                        maxLocalY = std::max(maxLocalY, y);
                    }
                }
            }
        }

        summary.nonAir = static_cast<uint16_t>(std::min<size_t>(nonAir, UINT16_MAX));
        if (nonAir > 0) {
            summary.minNonAirWorldY = chunkPos.y * CHUNK_SIZE + minLocalY;
            summary.maxNonAirWorldY = chunkPos.y * CHUNK_SIZE + maxLocalY;
        }
        return summary;
    }

    struct OrderedChunkOffset {
        int dx = 0;
        int y = 0;
        int dz = 0;
    };

    int VerticalStreamRank(int y, int verticalAnchorY) noexcept {
        const int dy = y - verticalAnchorY;
        if (dy == 0) {
            return 0;
        }
        if (dy == 1) {
            return 1;
        }
        if (dy == -1) {
            return 2;
        }
        return 2 + (std::abs(dy) * 2) + (dy < 0 ? 1 : 0);
    }

    int ChunkStreamPriority(const OrderedChunkOffset &offset, int verticalAnchorY) noexcept {
        const int horizDist2 = offset.dx * offset.dx + offset.dz * offset.dz;
        const int verticalRank = VerticalStreamRank(offset.y, verticalAnchorY);
        return (horizDist2 * 4) + (verticalRank * 3);
    }

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
                const int aPriority = ChunkStreamPriority(a, verticalAnchorY);
                const int bPriority = ChunkStreamPriority(b, verticalAnchorY);
                if (aPriority != bPriority) {
                    return aPriority < bPriority;
                }

                const int aHorizDist2 = a.dx * a.dx + a.dz * a.dz;
                const int bHorizDist2 = b.dx * b.dx + b.dz * b.dz;
                if (aHorizDist2 != bHorizDist2) {
                    return aHorizDist2 < bHorizDist2;
                }

                const int aVerticalRank = VerticalStreamRank(a.y, verticalAnchorY);
                const int bVerticalRank = VerticalStreamRank(b.y, verticalAnchorY);
                if (aVerticalRank != bVerticalRank) {
                    return aVerticalRank < bVerticalRank;
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
    constexpr int kDecorationReachChunksXZ = 2;
    constexpr int kDecorationReachChunksUp = 3;
    for (int dz = -kDecorationReachChunksXZ; dz <= kDecorationReachChunksXZ; ++dz) {
        for (int dx = -kDecorationReachChunksXZ; dx <= kDecorationReachChunksXZ; ++dx) {
            for (int dy = 0; dy <= kDecorationReachChunksUp; ++dy) {
                const glm::ivec3 sourcePos(coord.x + dx, coord.y - dy, coord.z + dz);
                if (m_chunkManager.inBounds(sourcePos) &&
                    m_chunkManager.loadOrGenerateChunk(sourcePos) == nullptr) {
                    return false;
                }
            }
        }
    }

    return m_chunkManager.isChunkStreamReady(glm::ivec3(coord.x, coord.y, coord.z));
}

bool ChunkStreamingService::QueueChunkPreparation(HSteamNetConnection conn, const ChunkCoord &coord) {
    ChunkPipelineState::QueuePrepResult result{};
    if (m_chunkManager.isChunkStreamReady(glm::ivec3(coord.x, coord.y, coord.z))) {
        result = m_pipelineState.QueuePreparedSend(conn, coord, kMaxChunkSendQueuePerClient);
    } else {
        result = m_pipelineState.QueuePrep(conn, coord, kMaxChunkPrepQueue);
        if (result == ChunkPipelineState::QueuePrepResult::Queued) {
            m_pipelineState.NotifyPrepWorker();
        }
    }

    return result == ChunkPipelineState::QueuePrepResult::Queued ||
           result == ChunkPipelineState::QueuePrepResult::AlreadyQueued;
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
        const bool queuedForSend = m_pipelineState.MarkPrepDoneAndQueueSend(
            task, prepared, m_chunkPrepQuit, kMaxChunkSendQueuePerClient
        );
        if (prepared && !queuedForSend) {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_sessionMutex, "ChunkStreamingService::ChunkPrepWorkerLoop.rescheduleSendFull"
            );
            auto it = m_sessions.find(task.conn);
            if (it != m_sessions.end()) {
                it->second.chunkInterestDirty = true;
                it->second.nextChunkInterestUpdateAt = std::chrono::steady_clock::time_point::min();
            }
        }
    }
}

ChunkPipelineState::QueuePrepBatchResult ChunkStreamingService::QueueChunkPreparations(
    HSteamNetConnection conn,
    const std::vector<ChunkCoord> &coords,
    std::vector<ChunkCoord> &acceptedCoords
) {
    ChunkPipelineState::QueuePrepBatchResult result{};
    bool queuedPrepTask = false;
    for (const ChunkCoord &coord : coords) {
        ChunkPipelineState::QueuePrepResult queueResult{};
        if (m_chunkManager.isChunkStreamReady(glm::ivec3(coord.x, coord.y, coord.z))) {
            queueResult = m_pipelineState.QueuePreparedSend(
                conn, coord, kMaxChunkSendQueuePerClient
            );
        } else {
            queueResult = m_pipelineState.QueuePrep(conn, coord, kMaxChunkPrepQueue);
            queuedPrepTask =
                queuedPrepTask || queueResult == ChunkPipelineState::QueuePrepResult::Queued;
        }

        if (queueResult == ChunkPipelineState::QueuePrepResult::QueueFull) {
            result.queueFull = true;
            break;
        }
        if (queueResult == ChunkPipelineState::QueuePrepResult::Queued ||
            queueResult == ChunkPipelineState::QueuePrepResult::AlreadyQueued) {
            acceptedCoords.push_back(coord);
            ++result.accepted;
        }
    }

    if (queuedPrepTask) {
        m_pipelineState.NotifyPrepWorkerAll();
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

bool ChunkStreamingService::CanSendChunkDataNow(HSteamNetConnection conn) const {
    constexpr int kMaxPendingReliableChunkBytes = 48 * 1024;

    SteamNetConnectionRealTimeStatus_t status{};
    const EResult result =
        SteamNetworkingSockets()->GetConnectionRealTimeStatus(conn, &status, 0, nullptr);
    if (result != k_EResultOK) {
        return true;
    }

    return status.m_cbPendingReliable < kMaxPendingReliableChunkBytes;
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

    size_t sent = 0;
    while (sent < maxSends) {
        if (!IsConnectionSendable(conn)) {
            ClearClientChunkState(conn);
            break;
        }
        if (!CanSendChunkDataNow(conn)) {
            break;
        }

        ChunkCoord coord{};
        if (!m_pipelineState.PopNextSendChunk(conn, coord)) {
            break;
        }

        bool stillPending = false;
        {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_sessionMutex, "ChunkStreamingService::FlushChunkSendQueueForClient.snapshotPending"
            );
            auto it = m_sessions.find(conn);
            if (it == m_sessions.end()) {
                return sent;
            }
            stillPending =
                it->second.pendingChunkData.find(coord) != it->second.pendingChunkData.end();
        }
        if (!stillPending) {
            continue;
        }

        const ChunkSendResult sendResult = SendChunkData(conn, coord);
        if (sendResult != ChunkSendResult::Sent) {
            if (!IsConnectionSendable(conn)) {
                ClearClientChunkState(conn);
            } else if (sendResult == ChunkSendResult::SendFailed) {
                const auto requeueResult =
                    m_pipelineState.RequeueSendFront(conn, coord, kMaxChunkSendQueuePerClient);
                if (requeueResult == ChunkPipelineState::QueuePrepResult::QueueFull) {
                    auto lk = LockWaitTelemetry::AcquireSessionLock(
                        m_sessionMutex,
                        "ChunkStreamingService::FlushChunkSendQueueForClient.requeueFull"
                    );
                    auto it = m_sessions.find(conn);
                    if (it != m_sessions.end()) {
                        it->second.chunkInterestDirty = true;
                        it->second.nextChunkInterestUpdateAt =
                            std::chrono::steady_clock::time_point::min();
                    }
                }
            } else {
                auto lk = LockWaitTelemetry::AcquireSessionLock(
                    m_sessionMutex,
                    "ChunkStreamingService::FlushChunkSendQueueForClient.missingChunk"
                );
                auto it = m_sessions.find(conn);
                if (it != m_sessions.end()) {
                    it->second.chunkInterestDirty = true;
                    it->second.nextChunkInterestUpdateAt =
                        std::chrono::steady_clock::time_point::min();
                }
            }
            break;
        }

        ++sent;
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

void ChunkStreamingService::ClearChunkPipelineForConnection(
    HSteamNetConnection conn, const ChunkCoord &coord
) {
    m_pipelineState.ClearChunkForConnection(conn, coord);
}

void ChunkStreamingService::ClearChunkPipelineForConnection(HSteamNetConnection conn) {
    m_pipelineState.ClearForConnection(conn);
}

ChunkStreamingService::ChunkSendResult
ChunkStreamingService::SendChunkData(HSteamNetConnection conn, const ChunkCoord &coord) {
    ServerChunk *chunk = m_chunkManager.loadOrGenerateChunk(glm::ivec3(coord.x, coord.y, coord.z));
    if (!chunk) {
        std::cerr << "[chunk/send] chunk missing after prep for conn=" << conn << " chunk=("
                  << coord.x << "," << coord.y << "," << coord.z << ")\n";
        return ChunkSendResult::MissingChunk;
    }

    ChunkData packet;
    packet.chunkX = coord.x;
    packet.chunkY = coord.y;
    packet.chunkZ = coord.z;
    std::vector<uint8_t> rawPayload(CHUNK_VOLUME * sizeof(BlockID));
    const int64_t snapshotVersion =
        chunk->fillRawVoxelBytesAndVersion(rawPayload.data(), rawPayload.size());
    packet.version = static_cast<uint64_t>(std::max<int64_t>(0, snapshotVersion));
    const RawChunkSummary rawSummary =
        SummarizeRawChunkPayload(rawPayload, glm::ivec3(coord.x, coord.y, coord.z));
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
        std::cerr << " retryingLater=1\n";
    } else if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire)) {
        std::cerr << "[chunk/send] summary conn=" << conn << " chunk=(" << coord.x << ","
                  << coord.y << "," << coord.z << ")"
                  << " version=" << packet.version << " rawHash=" << rawSummary.hash
                  << " nonAir=" << rawSummary.nonAir << " nonAirY=("
                  << rawSummary.minNonAirWorldY << "," << rawSummary.maxNonAirWorldY << ")"
                  << " flags=" << static_cast<int>(packet.flags)
                  << " payloadBytes=" << packet.payload.size() << "\n";
    }
    return result == k_EResultOK ? ChunkSendResult::Sent : ChunkSendResult::SendFailed;
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
    constexpr size_t kMaxChunkPrepQueuePerUpdate = 256;
    constexpr size_t kMaxChunkUnloadsPerUpdate = 24;
    constexpr auto kChunkRetryInterval = std::chrono::milliseconds(1000);
    const uint16_t clampedViewDistance = ClampViewDistance(viewDistance);
    const auto now = std::chrono::steady_clock::now();

    if (!IsConnectionSendable(conn)) {
        ClearClientChunkState(conn);
        return;
    }

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

    std::unordered_set<ChunkCoord, ChunkCoordHash> toUnloadSet;
    std::vector<ChunkCoord> toLoad;
    bool interestChanged = false;
    size_t pendingCount = 0;
    size_t streamedCount = 0;
    size_t desiredCount = 0;
    bool foundMoreLoadWork = false;
    struct PendingChunkRestore {
        ChunkCoord coord{};
        bool hadPrevious = false;
        std::chrono::steady_clock::time_point previousTime{};
    };
    std::vector<PendingChunkRestore> pendingChunkRestores;
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
        toLoad.reserve(std::min<size_t>(orderedOffsets.size(), kMaxChunkPrepQueuePerUpdate));
        for (const OrderedChunkOffset &offset : orderedOffsets) {
            const ChunkCoord c{centerChunk.x + offset.dx, offset.y, centerChunk.z + offset.dz};
            if (!IsDesiredChunk(c, desiredBounds)) {
                continue;
            }
            ++desiredCount;
            if (it->second.streamedChunks.find(c) != it->second.streamedChunks.end()) {
                continue;
            }

            const auto pendingIt = it->second.pendingChunkData.find(c);
            if (pendingIt != it->second.pendingChunkData.end() &&
                (now - pendingIt->second) < kChunkRetryInterval) {
                continue;
            }

            if (toLoad.size() >= kMaxChunkPrepQueuePerUpdate) {
                foundMoreLoadWork = true;
                break;
            }
            toLoad.push_back(c);
            const bool wasPending = pendingIt != it->second.pendingChunkData.end();
            pendingChunkRestores.push_back(PendingChunkRestore{
                c,
                wasPending,
                wasPending ? pendingIt->second : std::chrono::steady_clock::time_point{},
            });
            it->second.streamedChunks.erase(c);
            it->second.pendingChunkData[c] = now;
        }
        pendingCount = it->second.pendingChunkData.size();

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
    queuedPrepCoords.reserve(toLoad.size());

    for (const PendingChunkRestore &restore : pendingChunkRestores) {
        if (restore.hadPrevious) {
            m_pipelineState.ClearChunkForConnection(conn, restore.coord);
        }
    }

    if (!toLoad.empty()) {
        const auto prepResult = QueueChunkPreparations(conn, toLoad, queuedPrepCoords);
        queuedPrepThisUpdate = prepResult.accepted;
        stoppedByPrepCap = prepResult.queueFull;
    }

    if (queuedPrepThisUpdate < toLoad.size()) {
        std::unordered_set<ChunkCoord, ChunkCoordHash> acceptedCoordSet;
        acceptedCoordSet.reserve(queuedPrepCoords.size());
        for (const ChunkCoord &coord : queuedPrepCoords) {
            acceptedCoordSet.insert(coord);
        }

        auto lk = LockWaitTelemetry::AcquireSessionLock(
            m_sessionMutex, "ChunkStreamingService::UpdateChunkStreamingForClient.rollbackPending"
        );
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end() && it->second.interestCenterChunk == centerChunk &&
            it->second.viewDistance == clampedViewDistance) {
            for (const PendingChunkRestore &restore : pendingChunkRestores) {
                if (acceptedCoordSet.find(restore.coord) != acceptedCoordSet.end()) {
                    continue;
                }
                if (restore.hadPrevious) {
                    it->second.pendingChunkData[restore.coord] = restore.previousTime;
                } else {
                    it->second.pendingChunkData.erase(restore.coord);
                }
            }
            pendingCount = it->second.pendingChunkData.size();
        }
    }

    const size_t sendQueueDepth = GetChunkSendQueueDepthForClient(conn);

    static std::unordered_map<HSteamNetConnection, std::chrono::steady_clock::time_point>
        s_lastProgressLog;
    auto &lastLog = s_lastProgressLog[conn];

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        (now - lastLog) >= std::chrono::seconds(15)) {
        lastLog = now;
        std::cerr << "[chunk/stream] progress conn=" << conn << " desired=" << desiredCount
                  << " streamed=" << streamedCount << " pending=" << pendingCount
                  << " toLoad=" << toLoad.size() << " queuedPrepNow=" << queuedPrepThisUpdate
                  << " prepCapHit=" << (stoppedByPrepCap ? 1 : 0) << " sendQueue=" << sendQueueDepth
                  << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z
                  << ")"
                  << " viewDist=" << clampedViewDistance << "\n";
    }

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        !toLoad.empty() && queuedPrepThisUpdate == 0 && !stoppedByPrepCap &&
        sendQueueDepth == 0) {
        std::cerr << "[chunk/stream] stalled load window conn=" << conn
                  << " desired=" << desiredCount << " toLoad=" << toLoad.size()
                  << " streamed=" << streamedCount << " pending=" << pendingCount
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

    const bool hasPendingChunkWork = pendingCount > 0;
    const bool hasMoreInterestWork = hasPendingChunkWork || foundMoreLoadWork ||
                                     toLoad.size() > queuedPrepThisUpdate || stoppedByPrepCap ||
                                     toUnload.size() > unloadsSentThisUpdate;
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
