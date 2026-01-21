#pragma once

#include <array>
#include <cstdint>
#include <shared_mutex>
#include <deque>
#include <unordered_set>
#include <vector>
#include <string>
#include <chrono>
#include <optional>
#include <cstring>
#include <atomic>
#include <algorithm>

#include "Voxel.hpp"
#include <glm/vec3.hpp>

// keep same constants to stay compatible with client
constexpr int CHUNK_SIZE = 16;
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

// small edit op used in server edit log and diffs
struct EditOp {
    // coordinates are local to chunk (0..CHUNK_SIZE-1) to keep edits compact
    uint8_t x, y, z;
    BlockID newId;
    int64_t resultingVersion; // set when applied
};

using ClientId = uint64_t; // or whatever type you use to identify connections

class ServerChunk {
public:
    ServerChunk(glm::ivec3 pos = glm::ivec3(0));

    // Thread-safe accessors. These lock internally.
    BlockID getBlock(int x, int y, int z) const noexcept;
    BlockID getBlockUnchecked(int x, int y, int z) const noexcept;
    // Apply edit and return resulting version. Validates coordinates.
    int64_t applyEdit(int x, int y, int z, BlockID id);

    bool isCompletelyAir() const noexcept;

    // Return diffs since version (empty optional => too old to compute diff; request full chunk)
    std::optional<std::vector<EditOp>> diffSince(int64_t knownVersion, size_t maxOps = 1024) const;

    // Serialization: pack header + compressed data (you choose compression). 
    // These are thread-safe (lock inside).
    // serialize() returns raw bytes to send over network or to persist to disk.
    std::vector<uint8_t> serializeCompressed() const;
    // deserializeCompressed returns true on success (data becomes authoritative)
    bool deserializeCompressed(const std::vector<uint8_t>& blob);

    // persistence helpers (implement per your storage backend)
    bool loadFromDisk(const std::string& path);
    bool saveToDisk(const std::string& path) const;

    // subscription management (client connection identifiers)
    void addSubscriber(ClientId id);
    void removeSubscriber(ClientId id);
    std::vector<ClientId> getSubscribers() const;

    // metadata
    glm::ivec3 position;     // chunk coords
    int64_t version() const noexcept { return m_version.load(std::memory_order_acquire); }
    bool dirty() const noexcept { return m_dirty.load(std::memory_order_relaxed); }
    void markDirty() noexcept { m_dirty.store(true, std::memory_order_relaxed); }
    void clearDirty() noexcept { m_dirty.store(false, std::memory_order_relaxed); }

    // Return last access as steady_clock::time_point (constructed from stored ns)
    std::chrono::steady_clock::time_point getLastAccess() const noexcept {
        uint64_t ns = m_lastAccessNs.load(std::memory_order_relaxed);
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
    }

    // helpers to get world coordinates of chunk origin
    glm::ivec3 getWorldPosition() const noexcept { return position * CHUNK_SIZE; }

    // static helpers consistent with client
    static inline constexpr bool inBounds(int x, int y, int z) noexcept {
        return (unsigned)x < CHUNK_SIZE && (unsigned)y < CHUNK_SIZE && (unsigned)z < CHUNK_SIZE;
    }

private:
    // internal index arithmetic (same as client)
    static inline constexpr int idx(int x, int y, int z) noexcept {
        return x + CHUNK_SIZE * (y + CHUNK_SIZE * z);
    }

    // helper to get steady_clock now in ns (inline for header)
    static inline uint64_t nowNs() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
            );
    }

    // Raw voxel data (same memory layout as client). Using BlockID so server and client wire format match.
    std::array<BlockID, CHUNK_VOLUME> m_blocks;
    uint16_t m_nonAirCount = 0; // modified under write-lock

    // authority metadata
    mutable std::shared_mutex m_mutex; // shared for readers, exclusive for writers
    std::atomic<int64_t> m_version{ 0 };  // increment on every applied edit (atomic for lock-free reads)
    std::atomic<bool> m_dirty{ false };

    // bounded edit log to support diffs. Keep it reasonably sized.
    std::deque<EditOp> m_editLog;
    size_t m_maxEditLog = 8192; // tune this, ensures memory doesn't grow unlimited

    // subscribers: set of client ids who should receive diffs/edits for this chunk
    std::unordered_set<ClientId> m_subscribers;

    // last access for eviction heuristics: store as atomic nanoseconds from steady_clock epoch
    mutable std::atomic<uint64_t> m_lastAccessNs{ nowNs() };

    // update access time (can be called under shared_lock or unique_lock)
    void touchLockedAtomic() const noexcept { m_lastAccessNs.store(nowNs(), std::memory_order_relaxed); }

    // compression helpers: prefer LZ4; here we provide placeholder wrapper signatures
    static std::vector<uint8_t> compressBlob(const uint8_t* data, size_t size);
    static std::vector<uint8_t> decompressBlob(const uint8_t* data, size_t size);

    // only used by (de)serialization to get raw voxel bytes
    void fillRawVoxelBytes(uint8_t* outBuf, size_t bufSize) const;
    void loadRawVoxelBytes(const uint8_t* data, size_t bufSize);
};
