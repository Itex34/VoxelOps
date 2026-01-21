#pragma once
#include "../voxels/ServerChunk.hpp"
#include <functional>
#include <shared_mutex>
class ChunkStore {
public:
    ChunkStore(std::string worldDir, uint64_t seed);

    std::shared_ptr<ServerChunk> getOrLoad(glm::ivec3 chunkPos);
    std::shared_ptr<ServerChunk> tryGet(glm::ivec3 chunkPos);

    void saveDirty();
    void unloadUnused(std::chrono::seconds maxIdle);

    void forEachChunk(std::function<void(std::shared_ptr<ServerChunk>&)> fn);

private:
    std::unordered_map<int64_t, std::shared_ptr<ServerChunk>> m_chunks;
    mutable std::shared_mutex m_mutex;
    std::string m_worldDir;
    uint64_t m_seed;

    static int64_t key(glm::ivec3 p); // for unordered_map key
};
