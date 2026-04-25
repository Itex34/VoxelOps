#include "ServerNetwork.hpp"
#include "PacketParsers.hpp"

void ServerNetwork::HandleBlockPlaceRequestPacket(HSteamNetConnection incoming, const void *data,
                                                  uint32_t size) {
    auto sendResult = [&](const BlockPlaceResult &res) {
        const std::vector<uint8_t> bytes = res.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming, bytes.data(), static_cast<uint32_t>(bytes.size()),
            k_nSteamNetworkingSend_Reliable, nullptr);
    };

    auto buildCorrectiveChunks =
        [](const std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> &chunks) {
            std::vector<BlockPlaceChunkCoord> corrective;
            corrective.reserve(chunks.size());
            for (const glm::ivec3 &chunkPos : chunks) {
                corrective.push_back(BlockPlaceChunkCoord{chunkPos.x, chunkPos.y, chunkPos.z});
            }
            return corrective;
        };

    BlockPlaceRequest request{};
    if (!NetPacket::ParseBlockPlaceRequestPacket(reinterpret_cast<const uint8_t *>(data), size,
                                                 request)) {
        BlockPlaceResult result{};
        result.accepted = 0;
        result.rejectReason = BlockPlaceRejectReason::InvalidPacket;
        sendResult(result);
        std::cerr << "[block/place] malformed request size=" << size << "\n";
        return;
    }

    PlayerID requesterId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end() && !it->second.username.empty() && it->second.playerId != 0) {
            requesterId = it->second.playerId;
        }
    }
    if (requesterId == 0) {
        BlockPlaceResult result{};
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = BlockPlaceRejectReason::Unregistered;
        sendResult(result);
        return;
    }
    m_playerManager.touchHeartbeat(requesterId);

    // Deduplicate edits by world position (last write wins) and validate payload.
    std::unordered_map<glm::ivec3, BlockID, IVec3Hash, IVec3Eq> normalizedEdits;
    normalizedEdits.reserve(request.edits.size());
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> touchedChunks;
    touchedChunks.reserve(request.edits.size());
    for (const BlockPlaceEdit &edit : request.edits) {
        if (edit.blockId == static_cast<uint8_t>(BlockID::Air) ||
            edit.blockId >= static_cast<uint8_t>(BlockID::COUNT)) {
            BlockPlaceResult result{};
            result.requestId = request.requestId;
            result.accepted = 0;
            result.rejectReason = BlockPlaceRejectReason::InvalidPacket;
            result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
            sendResult(result);
            return;
        }

        const glm::ivec3 worldPos(edit.worldX, edit.worldY, edit.worldZ);
        const glm::ivec3 chunkPos = m_chunkManager.worldToChunkPos(worldPos);
        if (!m_chunkManager.inBounds(chunkPos)) {
            BlockPlaceResult result{};
            result.requestId = request.requestId;
            result.accepted = 0;
            result.rejectReason = BlockPlaceRejectReason::OutOfBounds;
            result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
            sendResult(result);
            return;
        }

        normalizedEdits[worldPos] = static_cast<BlockID>(edit.blockId);
        touchedChunks.insert(chunkPos);
    }

    if (normalizedEdits.empty()) {
        BlockPlaceResult result{};
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = BlockPlaceRejectReason::InvalidPacket;
        sendResult(result);
        return;
    }

    // Reject if any target cell intersects an alive player's collision capsule AABB.
    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    constexpr float kOccupancyEpsilon = 0.001f;
    auto playerOccupiesBlock = [&](const ServerPlayer &player, const glm::ivec3 &blockPos) {
        const float pxMin = player.position.x - player.radius + kOccupancyEpsilon;
        const float pxMax = player.position.x + player.radius - kOccupancyEpsilon;
        const float pyMin = player.position.y + kOccupancyEpsilon;
        const float pyMax = player.position.y + player.height - kOccupancyEpsilon;
        const float pzMin = player.position.z - player.radius + kOccupancyEpsilon;
        const float pzMax = player.position.z + player.radius - kOccupancyEpsilon;

        const float bxMin = static_cast<float>(blockPos.x);
        const float bxMax = bxMin + 1.0f;
        const float byMin = static_cast<float>(blockPos.y);
        const float byMax = byMin + 1.0f;
        const float bzMin = static_cast<float>(blockPos.z);
        const float bzMax = bzMin + 1.0f;

        return !(pxMax <= bxMin || pxMin >= bxMax || pyMax <= byMin || pyMin >= byMax ||
                 pzMax <= bzMin || pzMin >= bzMax);
    };

    for (const auto &entry : normalizedEdits) {
        const glm::ivec3 &worldPos = entry.first;
        for (const ServerPlayer &player : players) {
            if (!player.isAlive) {
                continue;
            }
            if (playerOccupiesBlock(player, worldPos)) {
                BlockPlaceResult result{};
                result.requestId = request.requestId;
                result.accepted = 0;
                result.rejectReason = BlockPlaceRejectReason::PlayerOccupied;
                result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
                sendResult(result);
                return;
            }
        }
    }

    struct ChunkDeltaAggregate {
        std::vector<ChunkDeltaOp> edits;
        uint64_t resultingVersion = 0;
    };
    std::unordered_map<glm::ivec3, ChunkDeltaAggregate, IVec3Hash, IVec3Eq> perChunkEdits;
    perChunkEdits.reserve(touchedChunks.size());

    for (const auto &[worldPos, newId] : normalizedEdits) {
        const BlockID oldId = m_chunkManager.getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
        if (oldId == newId) {
            continue;
        }

        m_chunkManager.setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, newId);

        const glm::ivec3 chunkPos = m_chunkManager.worldToChunkPos(worldPos);
        const glm::ivec3 localPos = m_chunkManager.worldToLocalPos(worldPos);
        ChunkDeltaAggregate &aggregate = perChunkEdits[chunkPos];
        aggregate.edits.push_back(
            ChunkDeltaOp{static_cast<uint8_t>(localPos.x), static_cast<uint8_t>(localPos.y),
                         static_cast<uint8_t>(localPos.z), static_cast<uint8_t>(newId)});
    }

    std::vector<ChunkDelta> outboundDeltas;
    outboundDeltas.reserve(perChunkEdits.size());
    for (auto &[chunkPos, aggregate] : perChunkEdits) {
        if (aggregate.edits.empty()) {
            continue;
        }
        ServerChunk *chunk = m_chunkManager.getChunkIfExists(chunkPos);
        if (chunk == nullptr) {
            BlockPlaceResult result{};
            result.requestId = request.requestId;
            result.accepted = 0;
            result.rejectReason = BlockPlaceRejectReason::ServerError;
            result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
            sendResult(result);
            return;
        }
        aggregate.resultingVersion = static_cast<uint64_t>(std::max<int64_t>(0, chunk->version()));

        ChunkDelta delta{};
        delta.chunkX = chunkPos.x;
        delta.chunkY = chunkPos.y;
        delta.chunkZ = chunkPos.z;
        delta.resultingVersion = aggregate.resultingVersion;
        delta.edits = std::move(aggregate.edits);
        outboundDeltas.push_back(std::move(delta));
    }

    for (const ChunkDelta &delta : outboundDeltas) {
        const ChunkCoord coord{delta.chunkX, delta.chunkY, delta.chunkZ};
        std::vector<HSteamNetConnection> recipients;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            recipients.reserve(m_clients.size());
            for (const auto &[conn, session] : m_clients) {
                if (session.streamedChunks.find(coord) != session.streamedChunks.end()) {
                    recipients.push_back(conn);
                }
            }
        }

        if (recipients.empty()) {
            continue;
        }

        const std::vector<uint8_t> bytes = delta.serialize();
        for (HSteamNetConnection conn : recipients) {
            (void)SteamNetworkingSockets()->SendMessageToConnection(
                conn, bytes.data(), static_cast<uint32_t>(bytes.size()),
                k_nSteamNetworkingSend_Reliable, nullptr);
        }
    }

    BlockPlaceResult result{};
    result.requestId = request.requestId;
    result.accepted = 1;
    result.rejectReason = BlockPlaceRejectReason::None;
    sendResult(result);
}

void ServerNetwork::HandleBlockBreakRequestPacket(HSteamNetConnection incoming, const void *data,
                                                  uint32_t size) {
    auto sendResult = [&](const BlockBreakResult &res) {
        const std::vector<uint8_t> bytes = res.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming, bytes.data(), static_cast<uint32_t>(bytes.size()),
            k_nSteamNetworkingSend_Reliable, nullptr);
    };

    auto buildCorrectiveChunks =
        [](const std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> &chunks) {
            std::vector<BlockBreakChunkCoord> corrective;
            corrective.reserve(chunks.size());
            for (const glm::ivec3 &chunkPos : chunks) {
                corrective.push_back(BlockBreakChunkCoord{chunkPos.x, chunkPos.y, chunkPos.z});
            }
            return corrective;
        };

    BlockBreakRequest request{};
    if (!NetPacket::ParseBlockBreakRequestPacket(reinterpret_cast<const uint8_t *>(data), size,
                                                 request)) {
        BlockBreakResult result{};
        result.accepted = 0;
        result.rejectReason = BlockBreakRejectReason::InvalidPacket;
        sendResult(result);
        std::cerr << "[block/break] malformed request size=" << size << "\n";
        return;
    }

    PlayerID requesterId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end() && !it->second.username.empty() && it->second.playerId != 0) {
            requesterId = it->second.playerId;
        }
    }
    if (requesterId == 0) {
        BlockBreakResult result{};
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = BlockBreakRejectReason::Unregistered;
        sendResult(result);
        return;
    }
    m_playerManager.touchHeartbeat(requesterId);

    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> normalizedEdits;
    normalizedEdits.reserve(request.edits.size());
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> touchedChunks;
    touchedChunks.reserve(request.edits.size());
    for (const BlockBreakEdit &edit : request.edits) {
        const glm::ivec3 worldPos(edit.worldX, edit.worldY, edit.worldZ);
        const glm::ivec3 chunkPos = m_chunkManager.worldToChunkPos(worldPos);
        if (!m_chunkManager.inBounds(chunkPos)) {
            BlockBreakResult result{};
            result.requestId = request.requestId;
            result.accepted = 0;
            result.rejectReason = BlockBreakRejectReason::OutOfBounds;
            result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
            sendResult(result);
            return;
        }

        normalizedEdits.insert(worldPos);
        touchedChunks.insert(chunkPos);
    }

    if (normalizedEdits.empty()) {
        BlockBreakResult result{};
        result.requestId = request.requestId;
        result.accepted = 0;
        result.rejectReason = BlockBreakRejectReason::InvalidPacket;
        sendResult(result);
        return;
    }

    // Keep break occupancy rules aligned with placement rules.
    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    constexpr float kOccupancyEpsilon = 0.001f;
    auto playerOccupiesBlock = [&](const ServerPlayer &player, const glm::ivec3 &blockPos) {
        const float pxMin = player.position.x - player.radius + kOccupancyEpsilon;
        const float pxMax = player.position.x + player.radius - kOccupancyEpsilon;
        const float pyMin = player.position.y + kOccupancyEpsilon;
        const float pyMax = player.position.y + player.height - kOccupancyEpsilon;
        const float pzMin = player.position.z - player.radius + kOccupancyEpsilon;
        const float pzMax = player.position.z + player.radius - kOccupancyEpsilon;

        const float bxMin = static_cast<float>(blockPos.x);
        const float bxMax = bxMin + 1.0f;
        const float byMin = static_cast<float>(blockPos.y);
        const float byMax = byMin + 1.0f;
        const float bzMin = static_cast<float>(blockPos.z);
        const float bzMax = bzMin + 1.0f;

        return !(pxMax <= bxMin || pxMin >= bxMax || pyMax <= byMin || pyMin >= byMax ||
                 pzMax <= bzMin || pzMin >= bzMax);
    };

    for (const glm::ivec3 &worldPos : normalizedEdits) {
        for (const ServerPlayer &player : players) {
            if (!player.isAlive) {
                continue;
            }
            if (playerOccupiesBlock(player, worldPos)) {
                BlockBreakResult result{};
                result.requestId = request.requestId;
                result.accepted = 0;
                result.rejectReason = BlockBreakRejectReason::PlayerOccupied;
                result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
                sendResult(result);
                return;
            }
        }
    }

    struct ChunkDeltaAggregate {
        std::vector<ChunkDeltaOp> edits;
        uint64_t resultingVersion = 0;
    };
    std::unordered_map<glm::ivec3, ChunkDeltaAggregate, IVec3Hash, IVec3Eq> perChunkEdits;
    perChunkEdits.reserve(touchedChunks.size());

    for (const glm::ivec3 &worldPos : normalizedEdits) {
        const BlockID oldId = m_chunkManager.getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
        if (oldId == BlockID::Air) {
            continue;
        }

        m_chunkManager.setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, BlockID::Air);

        const glm::ivec3 chunkPos = m_chunkManager.worldToChunkPos(worldPos);
        const glm::ivec3 localPos = m_chunkManager.worldToLocalPos(worldPos);
        ChunkDeltaAggregate &aggregate = perChunkEdits[chunkPos];
        aggregate.edits.push_back(
            ChunkDeltaOp{static_cast<uint8_t>(localPos.x), static_cast<uint8_t>(localPos.y),
                         static_cast<uint8_t>(localPos.z), static_cast<uint8_t>(BlockID::Air)});
    }

    std::vector<ChunkDelta> outboundDeltas;
    outboundDeltas.reserve(perChunkEdits.size());
    for (auto &[chunkPos, aggregate] : perChunkEdits) {
        if (aggregate.edits.empty()) {
            continue;
        }
        ServerChunk *chunk = m_chunkManager.getChunkIfExists(chunkPos);
        if (chunk == nullptr) {
            BlockBreakResult result{};
            result.requestId = request.requestId;
            result.accepted = 0;
            result.rejectReason = BlockBreakRejectReason::ServerError;
            result.correctiveChunks = buildCorrectiveChunks(touchedChunks);
            sendResult(result);
            return;
        }
        aggregate.resultingVersion = static_cast<uint64_t>(std::max<int64_t>(0, chunk->version()));

        ChunkDelta delta{};
        delta.chunkX = chunkPos.x;
        delta.chunkY = chunkPos.y;
        delta.chunkZ = chunkPos.z;
        delta.resultingVersion = aggregate.resultingVersion;
        delta.edits = std::move(aggregate.edits);
        outboundDeltas.push_back(std::move(delta));
    }

    for (const ChunkDelta &delta : outboundDeltas) {
        const ChunkCoord coord{delta.chunkX, delta.chunkY, delta.chunkZ};
        std::vector<HSteamNetConnection> recipients;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            recipients.reserve(m_clients.size());
            for (const auto &[conn, session] : m_clients) {
                if (session.streamedChunks.find(coord) != session.streamedChunks.end()) {
                    recipients.push_back(conn);
                }
            }
        }

        if (recipients.empty()) {
            continue;
        }

        const std::vector<uint8_t> bytes = delta.serialize();
        for (HSteamNetConnection conn : recipients) {
            (void)SteamNetworkingSockets()->SendMessageToConnection(
                conn, bytes.data(), static_cast<uint32_t>(bytes.size()),
                k_nSteamNetworkingSend_Reliable, nullptr);
        }
    }

    BlockBreakResult result{};
    result.requestId = request.requestId;
    result.accepted = 1;
    result.rejectReason = BlockBreakRejectReason::None;
    sendResult(result);
}
