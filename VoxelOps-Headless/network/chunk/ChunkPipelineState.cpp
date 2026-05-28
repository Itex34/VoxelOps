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

bool ChunkPipelineState::WaitPopPrepTask(ChunkPrepTask &outTask, const std::atomic<bool> &quitFlag) {
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

void ChunkPipelineState::PruneForClient(
    HSteamNetConnection conn, const std::unordered_set<ChunkCoord, ChunkCoordHash> &desired
) {
    std::lock_guard<std::mutex> lk(m_mutex);

    for (auto it = m_chunkPrepQueue.begin(); it != m_chunkPrepQueue.end();) {
        if (it->conn == conn && desired.find(it->coord) == desired.end()) {
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
            if (desired.find(*it) == desired.end()) {
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
