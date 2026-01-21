#include "ChunkStore.hpp"
#include <filesystem>
#include <thread>
#include <shared_mutex>
#include <vector>
#include <chrono>
#include <sstream>

static inline int64_t makeKey(glm::ivec3 p) {
    // pack 21-bit signed-ish coordinates into 63 bits; keep consistent with your decode logic
    return ((int64_t(p.x) & 0x1FFFFF)) |
        ((int64_t(p.y) & 0x1FFFFF) << 21) |
        ((int64_t(p.z) & 0x1FFFFF) << 42);
}

ChunkStore::ChunkStore(std::string dir, uint64_t seed)
    : m_worldDir(std::move(dir)), m_seed(seed)
{
    std::filesystem::create_directories(m_worldDir);
}

std::shared_ptr<ServerChunk> ChunkStore::tryGet(glm::ivec3 pos) {
    std::shared_lock lock(m_mutex);
    auto it = m_chunks.find(makeKey(pos));
    if (it == m_chunks.end()) return nullptr;
    return it->second;
}

std::shared_ptr<ServerChunk> ChunkStore::getOrLoad(glm::ivec3 pos) {
    int64_t k = makeKey(pos);

    // first, quick shared lookup
    {
        std::shared_lock lock(m_mutex);
        auto it = m_chunks.find(k);
        if (it != m_chunks.end()) return it->second;
    }

    // upgrade to exclusive: check again and load/insert
    {
        std::unique_lock lock(m_mutex);
        auto it = m_chunks.find(k);
        if (it != m_chunks.end()) return it->second;

        // create chunk
        auto chunk = std::make_shared<ServerChunk>(pos);

        // attempt to load from disk
        std::ostringstream ss;
        ss << m_worldDir << "/chunk_" << pos.x << "_" << pos.y << "_" << pos.z << ".bin";
        std::string path = ss.str();

        if (!chunk->loadFromDisk(path)) {
            // load failed -> leave chunk as default (all-air). Generation should be done by ChunkManager.
            // You could optionally call a generator callback here if you want ChunkStore to also generate.
        }

        m_chunks[k] = chunk;
        return chunk;
    }
}

void ChunkStore::saveDirty() {
    // Snapshot dirty chunks (avoid holding lock while saving to disk)
    struct ToSave { std::shared_ptr<ServerChunk> chunk; std::string path; };
    std::vector<ToSave> work;

    {
        std::shared_lock lock(m_mutex);
        work.reserve(m_chunks.size() / 4 + 1);
        for (const auto& kv : m_chunks) {
            const auto& chunk = kv.second;
            if (!chunk) continue;
            if (chunk->dirty()) {
                std::ostringstream ss;
                ss << m_worldDir << "/chunk_" << chunk->position.x << "_" << chunk->position.y << "_" << chunk->position.z << ".bin";
                work.push_back({ chunk, ss.str() });
            }
        }
    }

    // Perform I/O outside the lock
    for (auto& item : work) {
        // attempt save; ignore failures for now (could log)
        if (item.chunk->saveToDisk(item.path)) {
            // clear dirty flag (atomic)
            item.chunk->clearDirty();
        }
        else {
            // optional: log failure
            // std::cerr << "Failed to save chunk " << item.path << "\n";
        }
    }
}

void ChunkStore::unloadUnused(std::chrono::seconds maxIdle) {
    const auto now = std::chrono::steady_clock::now();

    // Move expired chunks out of map under lock so we can save them without holding the lock
    std::vector<std::shared_ptr<ServerChunk>> toSaveAndDestroy;
    toSaveAndDestroy.reserve(64);

    {
        std::unique_lock lock(m_mutex);
        for (auto it = m_chunks.begin(); it != m_chunks.end(); ) {
            const auto& chunk = it->second;
            if (!chunk) {
                it = m_chunks.erase(it);
                continue;
            }
            auto lastAccess = chunk->getLastAccess();
            auto age = now - lastAccess;
            if (age > maxIdle) {
                toSaveAndDestroy.push_back(chunk);
                it = m_chunks.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    // Save and destroy outside the lock
    for (auto& chunk : toSaveAndDestroy) {
        std::ostringstream ss;
        ss << m_worldDir << "/chunk_" << chunk->position.x << "_" << chunk->position.y << "_" << chunk->position.z << ".bin";
        std::string path = ss.str();

        // best-effort save
        if (chunk->saveToDisk(path)) {
            chunk->clearDirty();
        }
        else {
            // optional: log error
            // std::cerr << "Failed to save chunk on unload: " << path << "\n";
        }
        // shared_ptr goes out of scope and will free the chunk if no other refs
    }
}


void ChunkStore::forEachChunk(std::function<void(std::shared_ptr<ServerChunk>&)> fn) {
    std::shared_lock lock(m_mutex);
    for (auto& [key, chunk] : m_chunks) {
        fn(chunk);
    }
}

