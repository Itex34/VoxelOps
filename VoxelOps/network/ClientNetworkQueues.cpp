#include "ClientNetwork.hpp"

#include <utility>

bool ClientNetwork::PopChunkData(ChunkData &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_chunkStreamQueue.empty() ||
        m_chunkStreamQueue.front().type != ChunkStreamEventType::Data) {
        return false;
    }
    out = std::move(m_chunkStreamQueue.front().chunkData);
    DecrementChunkStreamEventDepth(m_chunkStreamQueue.front());
    m_chunkStreamQueue.pop_front();
    return true;
}

bool ClientNetwork::PopChunkDelta(ChunkDelta &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_chunkStreamQueue.empty() ||
        m_chunkStreamQueue.front().type != ChunkStreamEventType::Delta) {
        return false;
    }
    out = std::move(m_chunkStreamQueue.front().chunkDelta);
    DecrementChunkStreamEventDepth(m_chunkStreamQueue.front());
    m_chunkStreamQueue.pop_front();
    return true;
}

bool ClientNetwork::PopChunkUnload(ChunkUnload &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_chunkStreamQueue.empty() ||
        m_chunkStreamQueue.front().type != ChunkStreamEventType::Unload) {
        return false;
    }
    out = std::move(m_chunkStreamQueue.front().chunkUnload);
    DecrementChunkStreamEventDepth(m_chunkStreamQueue.front());
    m_chunkStreamQueue.pop_front();
    return true;
}

bool ClientNetwork::PeekChunkStreamEventType(ChunkStreamEventType &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_chunkStreamQueue.empty()) {
        return false;
    }
    out = m_chunkStreamQueue.front().type;
    return true;
}

bool ClientNetwork::PopChunkStreamEvent(ChunkStreamEvent &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_chunkStreamQueue.empty()) {
        return false;
    }
    out = std::move(m_chunkStreamQueue.front());
    DecrementChunkStreamEventDepth(m_chunkStreamQueue.front());
    m_chunkStreamQueue.pop_front();
    return true;
}

bool ClientNetwork::PopPlayerSnapshot(PlayerSnapshotFrame &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_playerSnapshotQueue.empty()) {
        return false;
    }
    out = std::move(m_playerSnapshotQueue.front());
    m_playerSnapshotQueue.pop_front();
    return true;
}

bool ClientNetwork::PopShootResult(ShootResult &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_shootResultQueue.empty()) {
        return false;
    }
    out = std::move(m_shootResultQueue.front());
    m_shootResultQueue.pop_front();
    return true;
}

bool ClientNetwork::PopGrappleResult(GrappleResult &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_grappleResultQueue.empty()) {
        return false;
    }
    out = std::move(m_grappleResultQueue.front());
    m_grappleResultQueue.pop_front();
    return true;
}

bool ClientNetwork::PopInventoryActionResult(InventoryActionResult &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_inventoryActionResultQueue.empty()) {
        return false;
    }
    out = std::move(m_inventoryActionResultQueue.front());
    m_inventoryActionResultQueue.pop_front();
    return true;
}

bool ClientNetwork::PopInventorySnapshot(InventorySnapshot &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_inventorySnapshotQueue.empty()) {
        return false;
    }
    out = std::move(m_inventorySnapshotQueue.front());
    m_inventorySnapshotQueue.pop_front();
    return true;
}

bool ClientNetwork::PopWorldItemSnapshot(WorldItemSnapshot &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_worldItemSnapshotQueue.empty()) {
        return false;
    }
    out = std::move(m_worldItemSnapshotQueue.front());
    m_worldItemSnapshotQueue.pop_front();
    return true;
}

bool ClientNetwork::PopBlockPlaceResult(BlockPlaceResult &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_blockPlaceResultQueue.empty()) {
        return false;
    }
    out = std::move(m_blockPlaceResultQueue.front());
    m_blockPlaceResultQueue.pop_front();
    return true;
}

bool ClientNetwork::PopBlockBreakResult(BlockBreakResult &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_blockBreakResultQueue.empty()) {
        return false;
    }
    out = std::move(m_blockBreakResultQueue.front());
    m_blockBreakResultQueue.pop_front();
    return true;
}

bool ClientNetwork::PopKillFeedEvent(KillFeedEvent &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_killFeedQueue.empty()) {
        return false;
    }
    out = std::move(m_killFeedQueue.front());
    m_killFeedQueue.pop_front();
    return true;
}

bool ClientNetwork::PopScoreboardSnapshot(ScoreboardSnapshot &out) {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    if (m_scoreboardQueue.empty()) {
        return false;
    }
    out = std::move(m_scoreboardQueue.front());
    m_scoreboardQueue.pop_front();
    return true;
}

ClientNetwork::ChunkQueueDepths ClientNetwork::GetChunkQueueDepths() {
    std::lock_guard<std::mutex> lk(m_inboundMutex);
    ChunkQueueDepths depths;
    depths.chunkData = m_chunkDataQueueDepth;
    depths.chunkDelta = m_chunkDeltaQueueDepth;
    depths.chunkUnload = m_chunkUnloadQueueDepth;
    return depths;
}
