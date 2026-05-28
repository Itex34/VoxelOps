#include "BlockEditExecutionService.hpp"

#include "../core/LockWaitTelemetry.hpp"
#include "../../../Shared/items/PlaceableBlockMapping.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <algorithm>
#include <optional>

namespace {
constexpr float kOccupancyEpsilon = 0.001f;

template <typename ChunkCoordT>
std::vector<ChunkCoordT>
BuildCorrectiveChunks(const std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> &chunks) {
    std::vector<ChunkCoordT> corrective;
    corrective.reserve(chunks.size());
    for (const glm::ivec3 &chunkPos : chunks) {
        corrective.push_back(ChunkCoordT{chunkPos.x, chunkPos.y, chunkPos.z});
    }
    return corrective;
}

template <typename ResultT, typename RejectReasonT, typename ChunkCoordT>
ResultT MakeRejectedEditResult(
    uint32_t requestId,
    RejectReasonT reason,
    const std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> &touchedChunks,
    bool includeCorrectiveChunks
) {
    ResultT result{};
    result.requestId = requestId;
    result.accepted = 0;
    result.rejectReason = reason;
    if (includeCorrectiveChunks) {
        result.correctiveChunks = BuildCorrectiveChunks<ChunkCoordT>(touchedChunks);
    }
    return result;
}
} // namespace

BlockEditExecutionService::BlockEditExecutionService(
    std::mutex &mutex,
    ClientSessionManager &sessions,
    PlayerManager &playerManager,
    ChunkManager &chunkManager
)
    : m_mutex(mutex)
    , m_sessions(sessions)
    , m_playerManager(playerManager)
    , m_chunkManager(chunkManager) {}

bool BlockEditExecutionService::IsAnyAlivePlayerOccupyingBlock(
    const std::vector<ServerPlayer> &players, const glm::ivec3 &worldPos
) const {
    for (const ServerPlayer &player : players) {
        if (!player.isAlive) {
            continue;
        }

        const float pxMin = player.position.x - player.radius + kOccupancyEpsilon;
        const float pxMax = player.position.x + player.radius - kOccupancyEpsilon;
        const float pyMin = player.position.y + kOccupancyEpsilon;
        const float pyMax = player.position.y + player.height - kOccupancyEpsilon;
        const float pzMin = player.position.z - player.radius + kOccupancyEpsilon;
        const float pzMax = player.position.z + player.radius - kOccupancyEpsilon;

        const float bxMin = static_cast<float>(worldPos.x);
        const float bxMax = bxMin + 1.0f;
        const float byMin = static_cast<float>(worldPos.y);
        const float byMax = byMin + 1.0f;
        const float bzMin = static_cast<float>(worldPos.z);
        const float bzMax = bzMin + 1.0f;

        if (!(pxMax <= bxMin || pxMin >= bxMax || pyMax <= byMin || pyMin >= byMax ||
              pzMax <= bzMin || pzMin >= bzMax)) {
            return true;
        }
    }
    return false;
}

bool BlockEditExecutionService::ApplyBlockEditsAndBuildDeltas(
    const std::unordered_map<glm::ivec3, BlockID, IVec3Hash, IVec3Eq> &normalizedEdits,
    std::vector<ChunkDelta> &outboundDeltas
) {
    struct ChunkDeltaAggregate {
        std::vector<ChunkDeltaOp> edits;
        uint64_t resultingVersion = 0;
    };

    std::unordered_map<glm::ivec3, ChunkDeltaAggregate, IVec3Hash, IVec3Eq> perChunkEdits;
    perChunkEdits.reserve(normalizedEdits.size());
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
            ChunkDeltaOp{
                static_cast<uint8_t>(localPos.x),
                static_cast<uint8_t>(localPos.y),
                static_cast<uint8_t>(localPos.z),
                static_cast<uint8_t>(newId)
            }
        );
    }

    outboundDeltas.clear();
    outboundDeltas.reserve(perChunkEdits.size());
    for (auto &[chunkPos, aggregate] : perChunkEdits) {
        if (aggregate.edits.empty()) {
            continue;
        }
        ServerChunk *chunk = m_chunkManager.getChunkIfExists(chunkPos);
        if (chunk == nullptr) {
            return false;
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

    return true;
}

void BlockEditExecutionService::BroadcastChunkDeltas(const std::vector<ChunkDelta> &outboundDeltas) {
    for (const ChunkDelta &delta : outboundDeltas) {
        const ChunkCoord coord{delta.chunkX, delta.chunkY, delta.chunkZ};
        std::vector<HSteamNetConnection> recipients;
        {
            auto lk = LockWaitTelemetry::AcquireSessionLock(
                m_mutex, "BlockEditExecutionService::BroadcastChunkDeltas"
            );
            recipients.reserve(m_sessions.size());
            for (const auto &[conn, session] : m_sessions) {
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
                conn,
                bytes.data(),
                static_cast<uint32_t>(bytes.size()),
                k_nSteamNetworkingSend_Reliable,
                nullptr
            );
        }
    }
}

BlockPlaceResult
BlockEditExecutionService::ExecuteBlockPlaceRequest(PlayerID requesterId, const BlockPlaceRequest &request) {
    std::unordered_map<glm::ivec3, BlockID, IVec3Hash, IVec3Eq> normalizedEdits;
    normalizedEdits.reserve(request.edits.size());
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> touchedChunks;
    touchedChunks.reserve(request.edits.size());
    std::unordered_map<uint16_t, uint16_t> requiredItemsById;
    requiredItemsById.reserve(request.edits.size());
    for (const BlockPlaceEdit &edit : request.edits) {
        if (edit.blockId == static_cast<uint8_t>(BlockID::Air) ||
            edit.blockId >= static_cast<uint8_t>(BlockID::COUNT)) {
            return MakeRejectedEditResult<
                BlockPlaceResult,
                BlockPlaceRejectReason,
                BlockPlaceChunkCoord>(
                request.requestId, BlockPlaceRejectReason::InvalidPacket, touchedChunks, true
            );
        }

        const glm::ivec3 worldPos(edit.worldX, edit.worldY, edit.worldZ);
        const glm::ivec3 chunkPos = m_chunkManager.worldToChunkPos(worldPos);
        if (!m_chunkManager.inBounds(chunkPos)) {
            return MakeRejectedEditResult<
                BlockPlaceResult,
                BlockPlaceRejectReason,
                BlockPlaceChunkCoord>(
                request.requestId, BlockPlaceRejectReason::OutOfBounds, touchedChunks, true
            );
        }

        const std::optional<uint16_t> requiredItemId =
            PlaceableBlockMapping::itemIdFromBlockId(edit.blockId);
        if (!requiredItemId.has_value()) {
            return MakeRejectedEditResult<
                BlockPlaceResult,
                BlockPlaceRejectReason,
                BlockPlaceChunkCoord>(
                request.requestId, BlockPlaceRejectReason::InvalidPacket, touchedChunks, true
            );
        }

        normalizedEdits[worldPos] = static_cast<BlockID>(edit.blockId);
        touchedChunks.insert(chunkPos);
        const auto requiredIt = requiredItemsById.find(*requiredItemId);
        if (requiredIt == requiredItemsById.end()) {
            requiredItemsById.emplace(*requiredItemId, 1u);
        } else {
            requiredIt->second = static_cast<uint16_t>(requiredIt->second + 1u);
        }
    }

    if (normalizedEdits.empty()) {
        return MakeRejectedEditResult<
            BlockPlaceResult,
            BlockPlaceRejectReason,
            BlockPlaceChunkCoord>(
            request.requestId, BlockPlaceRejectReason::InvalidPacket, touchedChunks, false
        );
    }

    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    for (const auto &[worldPos, newId] : normalizedEdits) {
        (void)newId;
        if (IsAnyAlivePlayerOccupyingBlock(players, worldPos)) {
            return MakeRejectedEditResult<
                BlockPlaceResult,
                BlockPlaceRejectReason,
                BlockPlaceChunkCoord>(
                request.requestId, BlockPlaceRejectReason::PlayerOccupied, touchedChunks, true
            );
        }
    }

    std::vector<std::pair<uint16_t, uint16_t>> requiredItems;
    requiredItems.reserve(requiredItemsById.size());
    for (const auto &[itemId, count] : requiredItemsById) {
        requiredItems.emplace_back(itemId, count);
    }
    if (!m_playerManager.consumeItemsFromInventory(requesterId, requiredItems, nullptr)) {
        return MakeRejectedEditResult<
            BlockPlaceResult,
            BlockPlaceRejectReason,
            BlockPlaceChunkCoord>(
            request.requestId, BlockPlaceRejectReason::InvalidPacket, touchedChunks, true
        );
    }

    std::vector<ChunkDelta> outboundDeltas;
    if (!ApplyBlockEditsAndBuildDeltas(normalizedEdits, outboundDeltas)) {
        for (const auto &[itemId, count] : requiredItems) {
            uint16_t acceptedQuantity = 0;
            (void)m_playerManager.appendItemsToInventory(
                requesterId, itemId, count, acceptedQuantity, nullptr
            );
        }
        return MakeRejectedEditResult<
            BlockPlaceResult,
            BlockPlaceRejectReason,
            BlockPlaceChunkCoord>(
            request.requestId, BlockPlaceRejectReason::ServerError, touchedChunks, true
        );
    }

    BroadcastChunkDeltas(outboundDeltas);

    BlockPlaceResult result{};
    result.requestId = request.requestId;
    result.accepted = 1;
    result.rejectReason = BlockPlaceRejectReason::None;
    return result;
}

BlockBreakResult
BlockEditExecutionService::ExecuteBlockBreakRequest(PlayerID requesterId, const BlockBreakRequest &request) {
    (void)requesterId;

    std::unordered_map<glm::ivec3, BlockID, IVec3Hash, IVec3Eq> normalizedEdits;
    normalizedEdits.reserve(request.edits.size());
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> touchedChunks;
    touchedChunks.reserve(request.edits.size());
    for (const BlockBreakEdit &edit : request.edits) {
        const glm::ivec3 worldPos(edit.worldX, edit.worldY, edit.worldZ);
        const glm::ivec3 chunkPos = m_chunkManager.worldToChunkPos(worldPos);
        if (!m_chunkManager.inBounds(chunkPos)) {
            return MakeRejectedEditResult<
                BlockBreakResult,
                BlockBreakRejectReason,
                BlockBreakChunkCoord>(
                request.requestId, BlockBreakRejectReason::OutOfBounds, touchedChunks, true
            );
        }

        normalizedEdits[worldPos] = BlockID::Air;
        touchedChunks.insert(chunkPos);
    }

    if (normalizedEdits.empty()) {
        return MakeRejectedEditResult<
            BlockBreakResult,
            BlockBreakRejectReason,
            BlockBreakChunkCoord>(
            request.requestId, BlockBreakRejectReason::InvalidPacket, touchedChunks, false
        );
    }

    const std::vector<ServerPlayer> players = m_playerManager.getAllPlayersCopy();
    for (const auto &[worldPos, newId] : normalizedEdits) {
        (void)newId;
        if (IsAnyAlivePlayerOccupyingBlock(players, worldPos)) {
            return MakeRejectedEditResult<
                BlockBreakResult,
                BlockBreakRejectReason,
                BlockBreakChunkCoord>(
                request.requestId, BlockBreakRejectReason::PlayerOccupied, touchedChunks, true
            );
        }
    }

    std::vector<ChunkDelta> outboundDeltas;
    if (!ApplyBlockEditsAndBuildDeltas(normalizedEdits, outboundDeltas)) {
        return MakeRejectedEditResult<
            BlockBreakResult,
            BlockBreakRejectReason,
            BlockBreakChunkCoord>(
            request.requestId, BlockBreakRejectReason::ServerError, touchedChunks, true
        );
    }

    BroadcastChunkDeltas(outboundDeltas);

    BlockBreakResult result{};
    result.requestId = request.requestId;
    result.accepted = 1;
    result.rejectReason = BlockBreakRejectReason::None;
    return result;
}
