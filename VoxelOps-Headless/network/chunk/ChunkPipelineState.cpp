#include "ChunkPipelineState.hpp"

void ChunkPipelineState::Clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_chunkPrepQueue.clear();
    m_chunkPrepQueued.clear();
    m_chunkSendQueues.clear();
    m_chunkSendQueued.clear();
}

ChunkPipelineState::QueuePrepResult
ChunkPipelineState::QueuePrep(HSteamNetConnection conn, const ChunkCoord &coord, size_t maxQueue) {
    const ChunkPipelineKey key{conn, coord};
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_chunkPrepQueued.find(key) != m_chunkPrepQueued.end() ||
        m_chunkSendQueued.find(key) != m_chunkSendQueued.end()) {
        return QueuePrepResult::AlreadyQueued;
    }
    if (m_chunkPrepQueue.size() >= maxQueue) {
        return QueuePrepResult::QueueFull;
    }
    m_chunkPrepQueue.push_back(ChunkPrepTask{conn, coord});
    m_chunkPrepQueued.insert(key);
    return QueuePrepResult::Queued;
}

ChunkPipelineState::QueuePrepBatchResult ChunkPipelineState::QueuePrepBatch(
    HSteamNetConnection conn,
    const std::vector<ChunkCoord> &coords,
    size_t maxQueue,
    std::vector<ChunkCoord> &acceptedCoords
) {
    QueuePrepBatchResult result{};
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const ChunkCoord &coord : coords) {
        const ChunkPipelineKey key{conn, coord};
        if (m_chunkPrepQueued.find(key) != m_chunkPrepQueued.end() ||
            m_chunkSendQueued.find(key) != m_chunkSendQueued.end()) {
            acceptedCoords.push_back(coord);
            ++result.accepted;
            continue;
        }
        if (m_chunkPrepQueue.size() >= maxQueue) {
            result.queueFull = true;
            break;
        }
        m_chunkPrepQueue.push_back(ChunkPrepTask{conn, coord});
        m_chunkPrepQueued.insert(key);
        acceptedCoords.push_back(coord);
        ++result.accepted;
    }
    return result;
}

ChunkPipelineState::QueuePrepBatchResult ChunkPipelineState::QueuePreparedSendBatch(
    HSteamNetConnection conn,
    const std::vector<ChunkCoord> &coords,
    size_t maxChunkSendQueuePerClient,
    std::vector<ChunkCoord> &acceptedCoords
) {
    QueuePrepBatchResult result{};
    std::lock_guard<std::mutex> lk(m_mutex);
    auto &sendQ = m_chunkSendQueues[conn];
    for (const ChunkCoord &coord : coords) {
        const ChunkPipelineKey key{conn, coord};
        if (m_chunkPrepQueued.find(key) != m_chunkPrepQueued.end() ||
            m_chunkSendQueued.find(key) != m_chunkSendQueued.end()) {
            acceptedCoords.push_back(coord);
            ++result.accepted;
            continue;
        }
        if (sendQ.size() >= maxChunkSendQueuePerClient) {
            result.queueFull = true;
            break;
        }
        sendQ.push_back(coord);
        m_chunkSendQueued.insert(key);
        acceptedCoords.push_back(coord);
        ++result.accepted;
    }
    if (sendQ.empty()) {
        m_chunkSendQueues.erase(conn);
    }
    return result;
}

bool ChunkPipelineState::WaitPopPrepTask(
    ChunkPrepTask &outTask, const std::atomic<bool> &quitFlag
) {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_chunkPrepCv.wait(lk, [&]() {
        return quitFlag.load(std::memory_order_acquire) || !m_chunkPrepQueue.empty();
    });
    if (quitFlag.load(std::memory_order_acquire) && m_chunkPrepQueue.empty()) {
        return false;
    }
    outTask = m_chunkPrepQueue.front();
    m_chunkPrepQueue.pop_front();
    return true;
}

void ChunkPipelineState::MarkPrepDoneAndQueueSend(
    const ChunkPrepTask &task,
    bool prepared,
    const std::atomic<bool> &quitFlag,
    size_t maxChunkSendQueuePerClient
) {
    const ChunkPipelineKey key{task.conn, task.coord};
    std::lock_guard<std::mutex> lk(m_mutex);
    m_chunkPrepQueued.erase(key);
    if (prepared && !quitFlag.load(std::memory_order_acquire) &&
        m_chunkSendQueued.find(key) == m_chunkSendQueued.end()) {
        auto &sendQ = m_chunkSendQueues[task.conn];
        if (sendQ.size() < maxChunkSendQueuePerClient) {
            sendQ.push_back(task.coord);
            m_chunkSendQueued.insert(key);
        }
    }
}

bool ChunkPipelineState::PopNextSendChunk(HSteamNetConnection conn, ChunkCoord &outCoord) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto qIt = m_chunkSendQueues.find(conn);
    if (qIt == m_chunkSendQueues.end() || qIt->second.empty()) {
        return false;
    }
    outCoord = qIt->second.front();
    qIt->second.pop_front();
    if (qIt->second.empty()) {
        m_chunkSendQueues.erase(qIt);
    }
    m_chunkSendQueued.erase(ChunkPipelineKey{conn, outCoord});
    return true;
}

size_t ChunkPipelineState::PopNextSendChunks(
    HSteamNetConnection conn, size_t maxChunks, std::vector<ChunkCoord> &outCoords
) {
    if (maxChunks == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    auto qIt = m_chunkSendQueues.find(conn);
    if (qIt == m_chunkSendQueues.end() || qIt->second.empty()) {
        return 0;
    }

    auto &sendQueue = qIt->second;
    const size_t startSize = outCoords.size();
    while (outCoords.size() - startSize < maxChunks && !sendQueue.empty()) {
        ChunkCoord coord = sendQueue.front();
        sendQueue.pop_front();
        m_chunkSendQueued.erase(ChunkPipelineKey{conn, coord});
        outCoords.push_back(coord);
    }

    if (sendQueue.empty()) {
        m_chunkSendQueues.erase(qIt);
    }
    return outCoords.size() - startSize;
}

void ChunkPipelineState::PruneForClient(
    HSteamNetConnection conn, const ChunkInterestBounds &desired
) {
    std::lock_guard<std::mutex> lk(m_mutex);

    for (auto it = m_chunkPrepQueue.begin(); it != m_chunkPrepQueue.end();) {
        if (it->conn == conn && !desired.contains(it->coord)) {
            m_chunkPrepQueued.erase(ChunkPipelineKey{conn, it->coord});
            it = m_chunkPrepQueue.erase(it);
        } else {
            ++it;
        }
    }

    auto sendIt = m_chunkSendQueues.find(conn);
    if (sendIt != m_chunkSendQueues.end()) {
        auto &sendQueue = sendIt->second;
        for (auto it = sendQueue.begin(); it != sendQueue.end();) {
            if (!desired.contains(*it)) {
                m_chunkSendQueued.erase(ChunkPipelineKey{conn, *it});
                it = sendQueue.erase(it);
            } else {
                ++it;
            }
        }
        if (sendQueue.empty()) {
            m_chunkSendQueues.erase(sendIt);
        }
    }
}

size_t ChunkPipelineState::GetSendQueueDepthForClient(HSteamNetConnection conn) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_chunkSendQueues.find(conn);
    if (it == m_chunkSendQueues.end()) {
        return 0;
    }
    return it->second.size();
}

void ChunkPipelineState::ClearForConnection(HSteamNetConnection conn) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_chunkSendQueues.erase(conn);

    for (auto it = m_chunkPrepQueue.begin(); it != m_chunkPrepQueue.end();) {
        if (it->conn == conn) {
            it = m_chunkPrepQueue.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_chunkPrepQueued.begin(); it != m_chunkPrepQueued.end();) {
        if (it->conn == conn) {
            it = m_chunkPrepQueued.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_chunkSendQueued.begin(); it != m_chunkSendQueued.end();) {
        if (it->conn == conn) {
            it = m_chunkSendQueued.erase(it);
        } else {
            ++it;
        }
    }
}

void ChunkPipelineState::NotifyPrepWorker() {
    m_chunkPrepCv.notify_one();
}

void ChunkPipelineState::NotifyPrepWorkerAll() {
    m_chunkPrepCv.notify_all();
}
