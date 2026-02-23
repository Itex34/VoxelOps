#include "ServerChunk.hpp"

#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include <iostream>
#include <iomanip>

// ---- constructor ----------------------------------------------------------------
ServerChunk::ServerChunk(glm::ivec3 pos)
    : position(pos)
{
    // initialize all to "air" (assume BlockID(0) == air)
    m_blocks.fill(static_cast<BlockID>(0));
    m_nonAirCount = 0;
    m_version.store(0);
    m_dirty.store(false);
    m_lastAccessNs.store(nowNs(), std::memory_order_relaxed);
}

// ---- accessors ------------------------------------------------------------------
// getBlock: shared lock for concurrent readers; updates last-access atomically
BlockID ServerChunk::getBlock(int x, int y, int z) const noexcept {
    if (!inBounds(x, y, z)) return static_cast<BlockID>(0);
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    touchLockedAtomic();
    return m_blocks[idx(x, y, z)];
}

BlockID ServerChunk::getBlockUnchecked(int x, int y, int z) const noexcept {
    // caller promises in-bounds; use shared lock for thread-safety
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    touchLockedAtomic();
    return m_blocks[idx(x, y, z)];
}

bool ServerChunk::isCompletelyAir() const noexcept {
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    touchLockedAtomic();
    return m_nonAirCount == 0;
}

// ---- edit application -----------------------------------------------------------
int64_t ServerChunk::applyEdit(int x, int y, int z, BlockID id) {
    if (!inBounds(x, y, z)) return m_version.load(std::memory_order_acquire);
    std::unique_lock<std::shared_mutex> lk(m_mutex);
    const int index = idx(x, y, z);
    BlockID prev = m_blocks[index];

    if (prev == id) {
        // Option: bump version even on no-op to keep monotonic operation ordering.
        // If you prefer not to change version for no-op, return current version instead.
        int64_t newv = m_version.fetch_add(1) + 1;
        touchLockedAtomic();
        m_dirty.store(true, std::memory_order_relaxed);
        return newv;
    }

    // update non-air count
    if (prev == static_cast<BlockID>(0) && id != static_cast<BlockID>(0)) ++m_nonAirCount;
    else if (prev != static_cast<BlockID>(0) && id == static_cast<BlockID>(0)) --m_nonAirCount;

    m_blocks[index] = id;

    int64_t newVersion = m_version.fetch_add(1) + 1;

    EditOp op;
    op.x = static_cast<uint8_t>(x);
    op.y = static_cast<uint8_t>(y);
    op.z = static_cast<uint8_t>(z);
    op.newId = id;
    op.resultingVersion = newVersion;

    m_editLog.push_back(op);
    if (m_editLog.size() > m_maxEditLog) m_editLog.pop_front();

    touchLockedAtomic();
    m_dirty.store(true, std::memory_order_relaxed);
    return newVersion;
}

// ---- diff generation -----------------------------------------------------------
std::optional<std::vector<EditOp>> ServerChunk::diffSince(int64_t knownVersion, size_t maxOps) const {
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    touchLockedAtomic();

    // If no edits, and knownVersion == current, return empty vector (no change)
    if (m_editLog.empty()) {
        if (knownVersion >= m_version.load(std::memory_order_acquire)) return std::vector<EditOp>{};
        // else: there were changes but log was empty? fallthrough to resync logic
    }

    if (!m_editLog.empty()) {
        int64_t oldestVersion = m_editLog.front().resultingVersion;
        // if client is older than the oldest recorded edit, can't produce diffs
        if (knownVersion < oldestVersion) {
            return std::nullopt;
        }
    }

    std::vector<EditOp> out;
    out.reserve(std::min(maxOps, m_editLog.size()));
    for (const auto& op : m_editLog) {
        if (op.resultingVersion > knownVersion) {
            out.push_back(op);
            if (out.size() >= maxOps) break;
        }
    }
    return out;
}

// ---- subscribers ---------------------------------------------------------------
void ServerChunk::addSubscriber(ClientId id) {
    std::unique_lock<std::shared_mutex> lk(m_mutex);
    m_subscribers.insert(id);
    touchLockedAtomic();
}

void ServerChunk::removeSubscriber(ClientId id) {
    std::unique_lock<std::shared_mutex> lk(m_mutex);
    m_subscribers.erase(id);
    touchLockedAtomic();
}

std::vector<ClientId> ServerChunk::getSubscribers() const {
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    // return a snapshot
    return std::vector<ClientId>(m_subscribers.begin(), m_subscribers.end());
}

// ---- serialization helpers -----------------------------------------------------
void ServerChunk::fillRawVoxelBytes(uint8_t* outBuf, size_t bufSize) const {
    const size_t need = CHUNK_VOLUME * sizeof(BlockID);
    if (bufSize < need) return;
    // safe copy under shared lock for consistency
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    std::memcpy(outBuf, m_blocks.data(), need);
}

void ServerChunk::loadRawVoxelBytes(const uint8_t* data, size_t bufSize) {
    const size_t need = CHUNK_VOLUME * sizeof(BlockID);
    if (bufSize < need) return;
    // must hold exclusive lock when mutating
    std::unique_lock<std::shared_mutex> lk(m_mutex);
    std::memcpy(m_blocks.data(), data, need);
    // recompute nonAirCount
    uint16_t count = 0;
    for (size_t i = 0; i < CHUNK_VOLUME; ++i) {
        if (m_blocks[i] != static_cast<BlockID>(0)) ++count;
    }
    m_nonAirCount = count;
}

// Note: for production replace compressBlob/decompressBlob with LZ4 (fast) or similar
std::vector<uint8_t> ServerChunk::compressBlob(const uint8_t* data, size_t size) {
    // PASS-THROUGH implementation: no compression. Replace with LZ4 for production.
    std::vector<uint8_t> out;
    out.resize(size);
    std::memcpy(out.data(), data, size);
    return out;
}

std::vector<uint8_t> ServerChunk::decompressBlob(const uint8_t* data, size_t size) {
    // PASS-THROUGH implementation: no compression. Replace with LZ4 for production.
    std::vector<uint8_t> out;
    out.resize(size);
    std::memcpy(out.data(), data, size);
    return out;
}

// Pack: [int32 cx][int32 cy][int32 cz][int64 version][uint8 flags][int32 dataSize][data...]
// flags bit0 = compressed (0 = no compression in current stub)
std::vector<uint8_t> ServerChunk::serializeCompressed() const {
    // use shared lock to snapshot blocks & version (shared read is OK)
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    touchLockedAtomic();

    const size_t rawSize = CHUNK_VOLUME * sizeof(BlockID);
    std::vector<uint8_t> raw(rawSize);
    std::memcpy(raw.data(), m_blocks.data(), rawSize);

    // compress (stub)
    std::vector<uint8_t> c = compressBlob(raw.data(), rawSize);

    // build packet
    std::vector<uint8_t> out;
    out.reserve(4 + 4 + 4 + 8 + 1 + 4 + c.size());
    auto append32 = [&out](int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        out.push_back(static_cast<uint8_t>((u >> 0) & 0xFF));
        out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
        };
    auto append64 = [&out](int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
        };

    append32(position.x);
    append32(position.y);
    append32(position.z);
    append64(m_version.load(std::memory_order_acquire));
    out.push_back(0); // flags: bit0=compressed (0 = no)
    append32(static_cast<int32_t>(c.size()));
    out.insert(out.end(), c.begin(), c.end());
    return out;
}

bool ServerChunk::deserializeCompressed(const std::vector<uint8_t>& blob) {
    if (blob.size() < (4 + 4 + 4 + 8 + 1 + 4)) return false;
    size_t offset = 0;
    auto read32 = [&](int32_t& v)->bool {
        if (offset + 4 > blob.size()) return false;
        uint32_t u = (uint32_t)blob[offset] | ((uint32_t)blob[offset + 1] << 8) | ((uint32_t)blob[offset + 2] << 16) | ((uint32_t)blob[offset + 3] << 24);
        v = static_cast<int32_t>(u);
        offset += 4;
        return true;
        };
    auto read64 = [&](int64_t& v)->bool {
        if (offset + 8 > blob.size()) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= (uint64_t)blob[offset + i] << (8 * i);
        v = static_cast<int64_t>(u);
        offset += 8;
        return true;
        };

    int32_t cx, cy, cz;
    if (!read32(cx)) return false;
    if (!read32(cy)) return false;
    if (!read32(cz)) return false;
    int64_t version;
    if (!read64(version)) return false;
    if (offset + 1 > blob.size()) return false;
    uint8_t flags = blob[offset++];
    int32_t dataSize;
    if (!read32(dataSize)) return false;
    if (dataSize < 0) return false;
    if (offset + static_cast<size_t>(dataSize) > blob.size()) return false;

    // decompress (stub)
    std::vector<uint8_t> decompressed = decompressBlob(blob.data() + offset, static_cast<size_t>(dataSize));
    if (decompressed.size() < CHUNK_VOLUME * sizeof(BlockID)) return false;

    {
        std::unique_lock<std::shared_mutex> lk(m_mutex);
        position = glm::ivec3(cx, cy, cz);
        const size_t rawSize = CHUNK_VOLUME * sizeof(BlockID);
        std::memcpy(m_blocks.data(), decompressed.data(), rawSize);
        uint16_t count = 0;
        for (size_t i = 0; i < CHUNK_VOLUME; ++i) {
            if (m_blocks[i] != static_cast<BlockID>(0)) {
                ++count;
            }
        }
        m_nonAirCount = count;
        m_version.store(version, std::memory_order_release);
        m_dirty.store(false, std::memory_order_relaxed);
        // note: we do not populate editLog from serialization; editLog is for runtime edits. clear it.
        m_editLog.clear();
        touchLockedAtomic();
    }
    return true;
}

// ---- disk persistence ----------------------------------------------------------
bool ServerChunk::saveToDisk(const std::string& path) const {
    auto blob = serializeCompressed();
    if (blob.empty()) return false;
    std::ofstream fout(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!fout) return false;
    fout.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    return !!fout;
}

bool ServerChunk::loadFromDisk(const std::string& path) {
    std::ifstream fin(path, std::ios::binary | std::ios::in);
    if (!fin) return false;
    fin.seekg(0, std::ios::end);
    std::streamoff sz = fin.tellg();
    if (sz <= 0) return false;
    fin.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    fin.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!fin) return false;
    return deserializeCompressed(buf);
}

