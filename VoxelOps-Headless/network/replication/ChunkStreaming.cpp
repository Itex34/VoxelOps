#include "../core/Runtime.hpp"
#include "../protocol/PacketParsers.hpp"
#include "../core/DiagnosticsFlags.hpp"

#include <algorithm>
#include <cmath>

static int FloorDiv(int a, int b) {
    int q = a / b;
    const int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

void Runtime::UpdateChunkStreamingForClient(HSteamNetConnection conn,
                                                  const glm::ivec3 &centerChunk,
                                                  uint16_t viewDistance) {
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
        static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1) * (maxChunkY - minChunkY + 1)));

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
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(conn);
        if (it == m_clients.end()) {
            return;
        }

        it->second.interestCenterChunk = centerChunk;
        it->second.viewDistance = clampedViewDistance;
        it->second.hasChunkInterest = true;

        for (auto pIt = it->second.pendingChunkData.begin();
             pIt != it->second.pendingChunkData.end();) {
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
        // Top-most chunk layers are often sparse; bias one layer down to prioritize terrain.
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
    size_t sentThisUpdate = 0;
    bool stoppedByPendingCap = false;
    bool stoppedByPrepCap = false;
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

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_clients.find(conn);
            if (it != m_clients.end()) {
                // Mark as pending as soon as chunk work is queued to enforce backpressure.
                const bool wasPending =
                    it->second.pendingChunkData.find(c) != it->second.pendingChunkData.end();
                it->second.pendingChunkData[c] = now;
                if (!wasPending) {
                    ++pendingCount;
                }
            }
        }
        ++queuedPrepThisUpdate;
    }

    const size_t sendQueueDepth = GetChunkSendQueueDepthForClient(conn);

    static std::unordered_map<HSteamNetConnection, std::chrono::steady_clock::time_point>
        s_lastProgressLog;
    auto &lastLog = s_lastProgressLog[conn];
    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        (now - lastLog) >= std::chrono::seconds(1)) {
        lastLog = now;
        std::cerr << "[chunk/stream] progress conn=" << conn << " desired=" << desired.size()
                  << " streamed=" << streamedCount << " pending=" << pendingCount
                  << " toLoad=" << toLoad.size() << " queuedPrepNow=" << queuedPrepThisUpdate
                  << " sentNow=" << sentThisUpdate
                  << " pendingCapHit=" << (stoppedByPendingCap ? 1 : 0)
                  << " prepCapHit=" << (stoppedByPrepCap ? 1 : 0) << " sendQueue=" << sendQueueDepth
                  << " center=(" << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z
                  << ")"
                  << " viewDist=" << clampedViewDistance << "\n";
    }

    if (DiagnosticsFlags::g_enableChunkDiagnostics.load(std::memory_order_acquire) &&
        !toLoad.empty() && queuedPrepThisUpdate == 0 && sentThisUpdate == 0) {
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
    for (const ChunkCoord &c : toUnload) {
        if (unloadsSentThisUpdate >= kMaxChunkUnloadsPerUpdate) {
            break;
        }
        if (!SendChunkUnload(conn, c)) {
            continue;
        }

        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(conn);
        if (it != m_clients.end()) {
            it->second.streamedChunks.erase(c);
            it->second.pendingChunkData.erase(c);
            ++unloadsSentThisUpdate;
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

void Runtime::HandlePlayerInputPacket(HSteamNetConnection incoming, const void *data,
                                            uint32_t size, uint64_t &playerInputPacketsThisLoop) {
    ++playerInputPacketsThisLoop;
    PlayerInput input{};
    if (!NetPacket::ParsePlayerInputPacket(reinterpret_cast<const uint8_t *>(data), size, input)) {
        std::cout << "[recv] malformed PlayerInput (size=" << size << ")\n";
        return;
    }

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
        m_playerManager.enqueuePlayerInput(playerId, input);
        m_playerManager.setEquippedWeapon(playerId, input.weaponId);
    } else {
        std::cout << "[input] unregistered conn = " << incoming << " tick = " << input.inputTick
                  << "\n";
    }
}

void Runtime::HandleChunkRequestPacket(HSteamNetConnection incoming, const void *data,
                                             uint32_t size, uint64_t &chunkRequestPacketsThisLoop) {
    ++chunkRequestPacketsThisLoop;
    ChunkRequest req{};
    if (!NetPacket::ParseChunkRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cout << "[recv] malformed ChunkRequest (size=" << size << ")\n";
        return;
    }

    const glm::ivec3 centerChunk(req.chunkX, req.chunkY, req.chunkZ);
    const uint16_t clampedViewDistance = ClampViewDistance(req.viewDistance);
    bool registered = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end() && !it->second.username.empty() && it->second.playerId != 0) {
            const auto now = std::chrono::steady_clock::now();
            const bool hadInterest = it->second.hasChunkInterest;
            const bool centerChanged = !hadInterest ||
                                       it->second.interestCenterChunk.x != centerChunk.x ||
                                       it->second.interestCenterChunk.y != centerChunk.y ||
                                       it->second.interestCenterChunk.z != centerChunk.z;
            const bool viewChanged = !hadInterest || it->second.viewDistance != clampedViewDistance;

            it->second.interestCenterChunk = centerChunk;
            it->second.viewDistance = clampedViewDistance;
            it->second.hasChunkInterest = true;

            if (centerChanged || viewChanged || now >= it->second.nextChunkInterestUpdateAt) {
                it->second.chunkInterestDirty = true;
                it->second.nextChunkInterestUpdateAt = std::chrono::steady_clock::time_point::min();
            }

            registered = true;
        }
    }
    if (!registered) {
        return;
    }
}
