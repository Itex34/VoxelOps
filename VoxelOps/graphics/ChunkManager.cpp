
#include <windows.h>
#include <psapi.h>


struct ProcessMemoryStatsMB {
    size_t privateMB = 0;
    size_t workingSetMB = 0;
};

static ProcessMemoryStatsMB getProcessMemoryMB() {
    ProcessMemoryStatsMB stats;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        stats.privateMB = size_t(pmc.PrivateUsage / (1024 * 1024));
        stats.workingSetMB = size_t(pmc.WorkingSetSize / (1024 * 1024));
    }
    return stats;
}

#include "ChunkManager.hpp"
#include "WorldGen.hpp"
#include "ChunkRenderSystem.hpp"
#include "../network/DecompressChunk.hpp"


#include "../player/Player.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <cstdint>

namespace {
bool readI32LE(const std::vector<uint8_t>& data, size_t& offset, int32_t& out)
{
    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t u = static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
    out = static_cast<int32_t>(u);
    offset += 4;
    return true;
}

bool readI64LE(const std::vector<uint8_t>& data, size_t& offset, int64_t& out)
{
    if (offset + 8 > data.size()) {
        return false;
    }
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
    }
    out = static_cast<int64_t>(u);
    offset += 8;
    return true;
}

uint32_t fnv1a32(const uint8_t* data, size_t size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint32_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}
}

//positions only cube used for wireframe debug
float cubeVertices[] = {
    0,0,0,  1,0,0,
    1,0,0,  1,1,0,
    1,1,0,  0,1,0,
    0,1,0,  0,0,0,

    0,0,1,  1,0,1,
    1,0,1,  1,1,1,
    1,1,1,  0,1,1,
    0,1,1,  0,0,1,

    0,0,0,  0,0,1,
    1,0,0,  1,0,1,
    1,1,0,  1,1,1,
    0,1,0,  0,1,1,
};



ChunkManager::ChunkManager(Renderer& renderer_) : renderer(renderer_){
    // wireframe/VBO/VAO
    glGenVertexArrays(1, &wireVAO);
    glGenBuffers(1, &wireVBO);
    glBindVertexArray(wireVAO);
    glBindBuffer(GL_ARRAY_BUFFER, wireVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), cubeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    debugShader.emplace("../../../../VoxelOps/shaders/debugVert.vert", "../../../../VoxelOps/shaders/debugFrag.frag");

    noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    noise.SetFrequency(0.009f); // controls "hilliness" (lower = smoother, higher = rougher)

    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    noise.SetSeed(std::rand());
    ChunkMeshBuilder::resetProfileSnapshot();


    
    
    // build the chunk storage with positions set using unordered_map keyed by glm::ivec3
    //chunkMap.clear();
}

void ChunkManager::renderChunks(
    Shader& shader,
    Frustum& frustum,
    Player& player,
    int maxRenderDistance
)
{
    ChunkRenderSystem::renderChunks(*this, shader, frustum, player, maxRenderDistance);
}










void ChunkManager::renderChunkBorders(glm::mat4& view, glm::mat4& projection) {
    ChunkRenderSystem::renderChunkBorders(*this, view, projection);
}



void ChunkManager::markChunkDirty(const glm::ivec3& pos) {
    if (!inBounds(pos)) return;
    auto it = chunkMap.find(pos);
    if (it == chunkMap.end()) return;

    it->second.dirty = true;
    if (m_dirtyChunkPending.insert(pos).second) {
        m_dirtyChunkQueue.push_back(pos);
    }
}

void ChunkManager::updateDirtyChunks() {
    while (!m_dirtyChunkQueue.empty()) {
        const glm::ivec3 pos = m_dirtyChunkQueue.front();
        m_dirtyChunkQueue.pop_front();
        m_dirtyChunkPending.erase(pos);

        auto it = chunkMap.find(pos);
        if (it == chunkMap.end()) continue;
        if (!it->second.dirty) continue;

        updateDirtyChunkAt(pos);
    }
}







void ChunkManager::updateChunks(const glm::ivec3& playerWorldPos, int renderDistance) {


    std::vector<glm::ivec3> toErase;
    std::unordered_set<glm::ivec2, IVec2Hash> columnsToRefresh;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> desired;

    glm::ivec3 playerChunk = worldToChunkPos(playerWorldPos);
    const int64_t radius2 = static_cast<int64_t>(renderDistance) * static_cast<int64_t>(renderDistance);

    const int minY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);

    for (int x = playerChunk.x - renderDistance; x <= playerChunk.x + renderDistance; ++x) {
        const int64_t dx = static_cast<int64_t>(x - playerChunk.x);
        const int64_t dx2 = dx * dx;
        for (int z = playerChunk.z - renderDistance; z <= playerChunk.z + renderDistance; ++z) {
            const int64_t dz = static_cast<int64_t>(z - playerChunk.z);
            if (dx2 + dz * dz > radius2) {
                continue;
            }
            for (int y = minY; y <= maxY; ++y) {
                const glm::ivec3 pos(x, y, z);
                if (!inBounds(pos)) {
                    continue;
                }
                desired.insert(pos);
            }
        }
    }



    for (auto& [pos, chunk] : chunkMap) {
        if (desired.find(pos) == desired.end()) {
            toErase.push_back(pos);
        }
    }

    for (auto& pos : toErase) {
        columnsToRefresh.insert(glm::ivec2(pos.x, pos.z));
        chunkMap.erase(pos);
        m_networkChunkVersions.erase(pos);
        chunkMeshes.erase(pos);
        m_dirtyChunkPending.erase(pos);
    }

    for (const auto& c : columnsToRefresh) {
        rebuildColumnSunCache(c.x, c.y);
    }

    for (auto& pos : desired) {
        if (chunkMap.find(pos) == chunkMap.end()) {
            WorldGen::generateChunkAt(*this, pos);

            static const glm::ivec3 dirs[6] = {
                { 1, 0, 0}, {-1, 0, 0},
                { 0, 1, 0}, { 0,-1, 0},
                { 0, 0, 1}, { 0, 0,-1}
            };
            for (auto d : dirs) {
                markChunkDirty(pos + d);
            }
        }
    }
}

bool ChunkManager::applyNetworkChunkData(const ChunkData& packet) {
    std::vector<uint8_t> decodedPayload;
    if (!DecompressChunkPayload(packet.flags, packet.payload, decodedPayload)) {
        std::cerr
            << "[chunk/apply] failed to decode payload flags=" << static_cast<int>(packet.flags)
            << " chunk=(" << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ << ")"
            << " payloadBytes=" << packet.payload.size() << "\n";
        return false;
    }

    const std::vector<uint8_t>& payload = decodedPayload;
    const uint32_t payloadHash = fnv1a32(packet.payload.data(), packet.payload.size());
    const size_t rawBlockBytes = CHUNK_VOLUME * sizeof(BlockID);
    const uint8_t* raw = nullptr;
    glm::ivec3 chunkPos(packet.chunkX, packet.chunkY, packet.chunkZ);
    uint64_t incomingVersion = packet.version;

    // payload format mirrors ServerChunk::serializeCompressed():
    // [cx:i32][cy:i32][cz:i32][version:i64][flags:u8][dataSize:i32][voxel bytes...]
    {
        size_t offset = 0;
        int32_t payloadX = 0;
        int32_t payloadY = 0;
        int32_t payloadZ = 0;
        int64_t payloadVersion = 0;
        int32_t dataSize = 0;

        bool validWrappedPayload = true;
        validWrappedPayload = validWrappedPayload && readI32LE(payload, offset, payloadX);
        validWrappedPayload = validWrappedPayload && readI32LE(payload, offset, payloadY);
        validWrappedPayload = validWrappedPayload && readI32LE(payload, offset, payloadZ);
        validWrappedPayload = validWrappedPayload && readI64LE(payload, offset, payloadVersion);
        if (validWrappedPayload) {
            if (offset + 1 > payload.size()) {
                validWrappedPayload = false;
            }
            else {
                const uint8_t flags = payload[offset++];
                if ((flags & ~0x1u) != 0u) {
                    validWrappedPayload = false;
                }
            }
        }
        validWrappedPayload = validWrappedPayload && readI32LE(payload, offset, dataSize);
        if (validWrappedPayload) {
            if (dataSize < 0) {
                validWrappedPayload = false;
            }
            else {
                const size_t size = static_cast<size_t>(dataSize);
                if (offset + size > payload.size() || size != rawBlockBytes) {
                    validWrappedPayload = false;
                }
            }
        }

        if (validWrappedPayload) {
            if (payloadX != packet.chunkX || payloadY != packet.chunkY || payloadZ != packet.chunkZ) {
                std::cerr
                    << "[chunk/apply] header mismatch packet=("
                    << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ
                    << ") payload=("
                    << payloadX << "," << payloadY << "," << payloadZ << ")\n";
                return false;
            }

            if (payloadVersion < 0) {
                std::cerr
                    << "[chunk/apply] invalid negative payload version chunk=("
                    << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ << ")"
                    << " version=" << payloadVersion << "\n";
                return false;
            }
            incomingVersion = static_cast<uint64_t>(payloadVersion);
            if (incomingVersion != packet.version) {
                std::cerr
                    << "[chunk/apply] packet version mismatch chunk=("
                    << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ << ")"
                    << " packetVersion=" << packet.version
                    << " payloadVersion=" << incomingVersion << "\n";
                return false;
            }
            raw = payload.data() + offset;
        }
    }

    if (!raw) {
        // Backward compatibility for payloads that only contain raw voxels.
        if (payload.size() != rawBlockBytes) {
            std::cerr
                << "[chunk/apply] invalid ChunkData payload size="
                << payload.size() << " expected=" << rawBlockBytes
                << " chunk=(" << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ << ")\n";
            return false;
        }
        std::cerr
            << "[chunk/apply] using raw fallback payload for chunk=("
            << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ << ")\n";
        raw = payload.data();
    }

    auto knownVersionIt = m_networkChunkVersions.find(chunkPos);
    if (knownVersionIt != m_networkChunkVersions.end() && incomingVersion <= knownVersionIt->second) {
        static uint64_t staleChunkDataCount = 0;
        ++staleChunkDataCount;
        if (staleChunkDataCount <= 20 || (staleChunkDataCount % 100) == 0) {
            std::cerr
                << "[chunk/apply] stale ChunkData ignored chunk=("
                << chunkPos.x << "," << chunkPos.y << "," << chunkPos.z << ")"
                << " incomingVersion=" << incomingVersion
                << " knownVersion=" << knownVersionIt->second
                << " count=" << staleChunkDataCount << "\n";
        }
        return true;
    }

    removeChunkMesh(chunkPos);
    chunkMap.erase(chunkPos);
    auto [chunkIt, inserted] = chunkMap.try_emplace(chunkPos, chunkPos);
    (void)inserted;
  
    Chunk& chunk = chunkIt->second;
    size_t nonAirCount = 0;
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                const size_t i = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_SIZE * z));
                const BlockID id = static_cast<BlockID>(raw[i]);
                if (id != BlockID::Air) {
                    chunk.setBlock(x, y, z, id);
                    ++nonAirCount;
                }
            }
        }
    }

    const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    if (chunkPos.y == minChunkY && nonAirCount < static_cast<size_t>(CHUNK_SIZE * CHUNK_SIZE)) {
        std::cerr
            << "[chunk/apply] suspicious low nonAir in bottom chunk chunk=("
            << chunkPos.x << "," << chunkPos.y << "," << chunkPos.z << ")"
            << " nonAir=" << nonAirCount
            << " payloadHash=" << payloadHash
            << " payloadBytes=" << payload.size() << "\n";
    }

    rebuildColumnSunCache(chunkPos.x, chunkPos.z);
    updateDirtyChunkAt(chunkPos);

    static const glm::ivec3 dirs[6] = {
        { 1, 0, 0 }, { -1, 0, 0 },
        { 0, 1, 0 }, { 0, -1, 0 },
        { 0, 0, 1 }, { 0, 0, -1 }
    };
    for (const glm::ivec3& d : dirs) {
        const glm::ivec3 n = chunkPos + d;
        if (chunkMap.find(n) != chunkMap.end()) {
            updateDirtyChunkAt(n);
        }
    }

    m_networkChunkVersions[chunkPos] = incomingVersion;
    return true;
}

NetworkChunkDeltaApplyResult ChunkManager::applyNetworkChunkDelta(const ChunkDelta& packet) {
    const glm::ivec3 chunkPos(packet.chunkX, packet.chunkY, packet.chunkZ);
    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) {
        static uint64_t missingChunkDeltaCount = 0;
        ++missingChunkDeltaCount;
        if (missingChunkDeltaCount <= 20 || (missingChunkDeltaCount % 100) == 0) {
            std::cerr
                << "[chunk/delta] received delta for missing chunk=("
                << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ
                << ") edits=" << packet.edits.size()
                << " count=" << missingChunkDeltaCount << "\n";
        }
        return NetworkChunkDeltaApplyResult::MissingBaseChunk;
    }

    const auto versionIt = m_networkChunkVersions.find(chunkPos);
    if (versionIt == m_networkChunkVersions.end()) {
        std::cerr
            << "[chunk/delta] missing base version for chunk=("
            << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ
            << ") resultingVersion=" << packet.resultingVersion << "\n";
        return NetworkChunkDeltaApplyResult::MissingBaseChunk;
    }

    const uint64_t knownVersion = versionIt->second;
    const uint64_t incomingVersion = packet.resultingVersion;
    if (incomingVersion <= knownVersion) {
        static uint64_t staleChunkDeltaCount = 0;
        ++staleChunkDeltaCount;
        if (staleChunkDeltaCount <= 20 || (staleChunkDeltaCount % 100) == 0) {
            std::cerr
                << "[chunk/delta] stale delta ignored chunk=("
                << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ
                << ") knownVersion=" << knownVersion
                << " incomingVersion=" << incomingVersion
                << " count=" << staleChunkDeltaCount << "\n";
        }
        return NetworkChunkDeltaApplyResult::StaleVersion;
    }

    constexpr uint64_t kNoopVersionSlack = 64;
    const uint64_t maxExpectedVersion =
        knownVersion + static_cast<uint64_t>(packet.edits.size()) + kNoopVersionSlack;
    if (!packet.edits.empty() && incomingVersion > maxExpectedVersion) {
        std::cerr
            << "[chunk/delta] version gap detected chunk=("
            << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ
            << ") knownVersion=" << knownVersion
            << " incomingVersion=" << incomingVersion
            << " edits=" << packet.edits.size() << "\n";
        return NetworkChunkDeltaApplyResult::VersionGap;
    }

    Chunk& chunk = it->second;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> rebuildSet;
    rebuildSet.insert(chunkPos);

    for (const ChunkDeltaOp& op : packet.edits) {
        if (!Chunk::inBounds(static_cast<int>(op.x), static_cast<int>(op.y), static_cast<int>(op.z))) {
            continue;
        }

        const BlockID newId = static_cast<BlockID>(op.blockId);
        const BlockID oldId = chunk.getBlock(static_cast<int>(op.x), static_cast<int>(op.y), static_cast<int>(op.z));
        if (oldId == newId) {
            continue;
        }

        chunk.setBlock(static_cast<int>(op.x), static_cast<int>(op.y), static_cast<int>(op.z), newId);

        const glm::ivec3 worldPos = chunk.getWorldPosition() + glm::ivec3(
            static_cast<int>(op.x),
            static_cast<int>(op.y),
            static_cast<int>(op.z)
        );
        updateColumnSunCacheForBlockChange(worldPos.x, worldPos.y, worldPos.z, oldId, newId);

        if (op.x == 0) rebuildSet.insert(chunkPos + glm::ivec3(-1, 0, 0));
        if (op.x == CHUNK_SIZE - 1) rebuildSet.insert(chunkPos + glm::ivec3(1, 0, 0));
        if (op.y == 0) rebuildSet.insert(chunkPos + glm::ivec3(0, -1, 0));
        if (op.y == CHUNK_SIZE - 1) rebuildSet.insert(chunkPos + glm::ivec3(0, 1, 0));
        if (op.z == 0) rebuildSet.insert(chunkPos + glm::ivec3(0, 0, -1));
        if (op.z == CHUNK_SIZE - 1) rebuildSet.insert(chunkPos + glm::ivec3(0, 0, 1));
    }

    for (const glm::ivec3& pos : rebuildSet) {
        if (chunkMap.find(pos) != chunkMap.end()) {
            updateDirtyChunkAt(pos);
        }
    }

    m_networkChunkVersions[chunkPos] = incomingVersion;
    return NetworkChunkDeltaApplyResult::Applied;
}

void ChunkManager::applyNetworkChunkUnload(const ChunkUnload& packet) {
    const glm::ivec3 chunkPos(packet.chunkX, packet.chunkY, packet.chunkZ);
    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) {
        m_networkChunkVersions.erase(chunkPos);
        static uint64_t missingChunkUnloadCount = 0;
        ++missingChunkUnloadCount;
        if (missingChunkUnloadCount <= 20 || (missingChunkUnloadCount % 100) == 0) {
            std::cerr
                << "[chunk/unload] unload for missing chunk=("
                << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ
                << ") count=" << missingChunkUnloadCount << "\n";
        }
        return;
    }

    chunkMap.erase(it);
    m_networkChunkVersions.erase(chunkPos);
    removeChunkMesh(chunkPos);
    m_dirtyChunkPending.erase(chunkPos);
    rebuildColumnSunCache(chunkPos.x, chunkPos.z);

    static const glm::ivec3 dirs[6] = {
        { 1, 0, 0 }, { -1, 0, 0 },
        { 0, 1, 0 }, { 0, -1, 0 },
        { 0, 0, 1 }, { 0, 0, -1 }
    };
    for (const glm::ivec3& d : dirs) {
        const glm::ivec3 n = chunkPos + d;
        if (chunkMap.find(n) != chunkMap.end()) {
            updateDirtyChunkAt(n);
        }
    }
}


void ChunkManager::setBlockInWorld(const glm::ivec3& worldPos, BlockID blockID) {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    if (!inBounds(chunkPos)) return;

    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) return;

    Chunk& chunk = it->second;
    BlockID oldId = chunk.getBlock(localPos.x, localPos.y, localPos.z);
    if (oldId == blockID) return;
    chunk.setBlock(localPos.x, localPos.y, localPos.z, blockID);
    updateColumnSunCacheForBlockChange(worldPos.x, worldPos.y, worldPos.z, oldId, blockID);
    markChunkDirty(chunkPos);

    // mark neighbors if we touched an edge
    if (localPos.x == 0) markChunkDirty(chunkPos + glm::ivec3(-1, 0, 0));
    if (localPos.x == CHUNK_SIZE - 1) markChunkDirty(chunkPos + glm::ivec3(1, 0, 0));
    if (localPos.y == 0) markChunkDirty(chunkPos + glm::ivec3(0, -1, 0));
    if (localPos.y == CHUNK_SIZE - 1) markChunkDirty(chunkPos + glm::ivec3(0, 1, 0));
    if (localPos.z == 0) markChunkDirty(chunkPos + glm::ivec3(0, 0, -1));
    if (localPos.z == CHUNK_SIZE - 1) markChunkDirty(chunkPos + glm::ivec3(0, 0, 1));
}

glm::ivec3 ChunkManager::worldToChunkPos(const glm::ivec3& worldPos) const {
    // floor division to get the chunk indices (works for negatives)
    glm::vec3 f = glm::floor(glm::vec3(worldPos) / float(CHUNK_SIZE));
    return glm::ivec3(static_cast<int>(f.x), static_cast<int>(f.y), static_cast<int>(f.z));
}

glm::ivec3 ChunkManager::worldToLocalPos(const glm::ivec3& worldPos) const {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 local = worldPos - chunkPos * CHUNK_SIZE;
    return local;
}

bool ChunkManager::inBounds(const glm::ivec3& pos) const {
    const int minChunkY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    return pos.x >= WORLD_MIN_X && pos.x <= WORLD_MAX_X &&
        pos.y >= minChunkY && pos.y <= maxChunkY &&
        pos.z >= WORLD_MIN_Z && pos.z <= WORLD_MAX_Z;
}



void ChunkManager::setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id) {
    glm::ivec3 worldPos(worldX, worldY, worldZ);
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        BlockID oldId = it->second.getBlock(localPos.x, localPos.y, localPos.z);
        if (oldId == id) return;
        it->second.setBlock(localPos.x, localPos.y, localPos.z, id);
        updateColumnSunCacheForBlockChange(worldX, worldY, worldZ, oldId, id);
        markChunkDirty(chunkPos);
    }
}

BlockID ChunkManager::getBlockGlobal(int worldX, int worldY, int worldZ) {
    glm::ivec3 worldPos(worldX, worldY, worldZ);
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        return it->second.getBlock(localPos.x, localPos.y, localPos.z);
    }
    return BlockID::Air;
}









void ChunkManager::setBlockSafe(Chunk& currentChunk, const glm::ivec3& pos, BlockID id) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE &&
        pos.y >= 0 && pos.y < CHUNK_SIZE &&
        pos.z >= 0 && pos.z < CHUNK_SIZE) {
        BlockID oldId = currentChunk.getBlock(pos.x, pos.y, pos.z);
        if (oldId == id) return;
        currentChunk.setBlock(pos.x, pos.y, pos.z, id);
        const glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        updateColumnSunCacheForBlockChange(worldPos.x, worldPos.y, worldPos.z, oldId, id);
    }
    else {
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, id);
    }
}

BlockID ChunkManager::getBlockSafe(Chunk& currentChunk, const glm::ivec3& pos) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE &&
        pos.y >= 0 && pos.y < CHUNK_SIZE &&
        pos.z >= 0 && pos.z < CHUNK_SIZE) {
        return currentChunk.getBlock(pos.x, pos.y, pos.z);
    }
    else {
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        return getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
    }
}



void ChunkManager::debugMemoryEstimate()
{
    std::cout << "---- MEMORY ESTIMATE ----\n";

    const ProcessMemoryStatsMB mem = getProcessMemoryMB();
    std::cout << "Process private bytes (MB): "
        << mem.privateMB << "\n";
    std::cout << "Process working set (MB): "
        << mem.workingSetMB << "\n";

    std::cout << "sizeof(Chunk): "
        << sizeof(Chunk) << " bytes\n";

    std::cout << "chunkMap.size(): "
        << chunkMap.size() << "\n";

    double chunkMB =
        chunkMap.size() * sizeof(Chunk) / (1024.0 * 1024.0);

    std::cout << "estimated raw chunk bytes: "
        << chunkMB << " MB\n";

    std::cout << "chunkMeshes.size(): "
        << chunkMeshes.size() << "\n";

    const MeshBuildProfileSnapshot p = ChunkMeshBuilder::getProfileSnapshot();
    if (p.chunksMeshed > 0 && p.totalUs > 0) {
        const double invChunks = 1.0 / double(p.chunksMeshed);
        const double avgTotal = double(p.totalUs) * invChunks;
        const auto pct = [&](uint64_t us) { return (100.0 * double(us)) / double(p.totalUs); };
        const uint64_t profiledUs =
            p.blockGridUs +
            p.solidCacheUs +
            p.sunlightPrepUs +
            p.aoPrepUs +
            p.maskTransitionUs +
            p.maskLightingUs +
            p.greedyEmitUs;
        const uint64_t otherUs = (p.totalUs > profiledUs) ? (p.totalUs - profiledUs) : 0;
        std::cout << "Mesher profile (" << p.chunksMeshed << " chunks):\n";
        std::cout << "  avg total: " << avgTotal << " us/chunk\n";
        std::cout << "  block grid: " << (double(p.blockGridUs) * invChunks) << " us (" << pct(p.blockGridUs) << "%)\n";
        std::cout << "  solid cache: " << (double(p.solidCacheUs) * invChunks) << " us (" << pct(p.solidCacheUs) << "%)\n";
        std::cout << "  sunlight prep: " << (double(p.sunlightPrepUs) * invChunks) << " us (" << pct(p.sunlightPrepUs) << "%)\n";
        std::cout << "  AO prep: " << (double(p.aoPrepUs) * invChunks) << " us (" << pct(p.aoPrepUs) << "%)\n";
        std::cout << "  mask transitions: " << (double(p.maskTransitionUs) * invChunks) << " us (" << pct(p.maskTransitionUs) << "%)\n";
        std::cout << "  mask lighting: " << (double(p.maskLightingUs) * invChunks) << " us (" << pct(p.maskLightingUs) << "%)\n";
        std::cout << "  mask build: " << (double(p.maskBuildUs) * invChunks) << " us (" << pct(p.maskBuildUs) << "%)\n";
        std::cout << "  greedy emit: " << (double(p.greedyEmitUs) * invChunks) << " us (" << pct(p.greedyEmitUs) << "%)\n";
        std::cout << "  other/unprofiled: " << (double(otherUs) * invChunks) << " us (" << pct(otherUs) << "%)\n";
    }
}





void ChunkManager::playerBreakBlockAt(const glm::ivec3& blockCoords) {
    glm::ivec3 chunkPos = worldToChunkPos(blockCoords);
    glm::ivec3 localPos = worldToLocalPos(blockCoords);
    bool changed = false;

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        BlockID oldId = it->second.removeBlock(localPos.x, localPos.y, localPos.z);
        if (oldId != BlockID::Air) {
            updateColumnSunCacheForBlockChange(blockCoords.x, blockCoords.y, blockCoords.z, oldId, BlockID::Air);
            changed = true;
        }
    }

    if (!changed) return;

    updateDirtyChunkAt(chunkPos);

    // order matches isEdgeBlock: { x==0, x==15, y==0, y==15, z==0, z==15 }
    const std::array<glm::ivec3, 6> neighborOffsets = {
        glm::ivec3(-1,  0,  0), // x==0 -> neighbor x-1
        glm::ivec3(+1,  0,  0), // x==15 -> neighbor x+1
        glm::ivec3(0, -1,  0), // y==0 -> y-1
        glm::ivec3(0, +1,  0), // y==15 -> y+1
        glm::ivec3(0,  0, -1), // z==0 -> z-1
        glm::ivec3(0,  0, +1)  // z==15 -> z+1
    };

    auto edges = isEdgeBlock(localPos);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!edges[i]) continue;
        glm::ivec3 neighborChunk = chunkPos + neighborOffsets[i];

        if (chunkMap.find(neighborChunk) != chunkMap.end()) {
            updateDirtyChunkAt(neighborChunk);
        }
    }

    debugMemoryEstimate();
}

void ChunkManager::playerPlaceBlockAt(glm::ivec3 blockCoords, int faceNormal, BlockID blockType) {
    (void)faceNormal;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToRebuild;

    const auto queueChunkAndEdgeNeighbors = [&](const glm::ivec3& worldPos) {
        const glm::ivec3 chunkPos = worldToChunkPos(worldPos);
        const glm::ivec3 localPos = worldToLocalPos(worldPos);

        chunksToRebuild.insert(chunkPos);
        if (localPos.x == 0) chunksToRebuild.insert(chunkPos + glm::ivec3(-1, 0, 0));
        if (localPos.x == CHUNK_SIZE - 1) chunksToRebuild.insert(chunkPos + glm::ivec3(1, 0, 0));
        if (localPos.y == 0) chunksToRebuild.insert(chunkPos + glm::ivec3(0, -1, 0));
        if (localPos.y == CHUNK_SIZE - 1) chunksToRebuild.insert(chunkPos + glm::ivec3(0, 1, 0));
        if (localPos.z == 0) chunksToRebuild.insert(chunkPos + glm::ivec3(0, 0, -1));
        if (localPos.z == CHUNK_SIZE - 1) chunksToRebuild.insert(chunkPos + glm::ivec3(0, 0, 1));
    };

    // Place a 3x3 wall and only queue rebuilds for actually changed blocks.
    for (int x = 0; x < 3; ++x) {
        for (int y = 0; y < 3; ++y) {
            const glm::ivec3 worldPos(blockCoords.x + x, blockCoords.y + y, blockCoords.z);
            if (getBlockGlobal(worldPos.x, worldPos.y, worldPos.z) == blockType) {
                continue;
            }
            setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, blockType);
            queueChunkAndEdgeNeighbors(worldPos);
        }
    }

    for (const auto& pos : chunksToRebuild) {
        updateDirtyChunkAt(pos);
    }
}



void ChunkManager::updateDirtyChunkAt(const glm::ivec3& chunkPos) {
    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) return;

    Chunk& chunk = it->second;

    auto findChunk = [&](const glm::ivec3& pos) -> const Chunk* {
        auto it = chunkMap.find(pos);
        return (it != chunkMap.end()) ? &it->second : nullptr;
        };

    const Chunk* neighbors[6] = {};

    constexpr glm::ivec3 offsets[6] = {
        {1,0,0},{-1,0,0},
        {0,1,0},{0,-1,0},
        {0,0,1},{0,0,-1}
    };

    for (int i = 0; i < 6; ++i)
        neighbors[i] = findChunk(chunkPos + offsets[i]);




    auto start = std::chrono::high_resolution_clock::now();

    auto built = builder.buildChunkMesh(
        chunk, neighbors, chunkPos, atlas, enableAO, enableShadows,
        [this](int wx, int wz) { return this->getColumnTopOccluderY(wx, wz); }
    );

    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::micro> elapsed = end - start;

    //std::cout << "Chunk meshed in : " << elapsed.count() << " microseconds" << '\n';

    uploadChunkMesh(chunkPos, built.vertices, built.indices);

    chunk.dirty = false;
}




void ChunkManager::requestChunkRebuild(const glm::ivec3& pos) {
    auto it = chunkMap.find(pos);
    if (it == chunkMap.end()) return;

    Chunk& chunk = it->second;

    bool expected = false;
    if (!chunk.building.compare_exchange_strong(expected, true))
        return;  // already building → skip duplicate builds

    // enqueue CPU mesh build
    meshPool.enqueue([this, pos] {
        this->buildChunkMeshWorker(pos);
        });
}


void ChunkManager::buildChunkMeshWorker(glm::ivec3 pos) {
    std::thread{

    };
}











//region management

Region& ChunkManager::getOrCreateRegion(const glm::ivec3& chunkPos) {
    glm::ivec3 regionPos = chunkToRegionPos(chunkPos);

    auto it = regions.find(regionPos);
    if (it != regions.end()) {
        return it->second;
    }

    // Create new region
    auto [newIt, inserted] = regions.emplace(
        regionPos,
        Region(regionPos, REGION_VERTEX_BYTES, REGION_INDEX_BYTES)
    );

    std::cout << "[ChunkManager] Created region at ("
        << regionPos.x << ", " << regionPos.y << ", " << regionPos.z << ")\n";

    return newIt->second;
}




void ChunkManager::uploadChunkMesh(
    const glm::ivec3& chunkPos,
    const std::vector<VoxelVertex>& vertices,
    const std::vector<uint16_t>& indices)
{
    Region& region = getOrCreateRegion(chunkPos);

    // remove old mesh if present
    auto old = region.chunks.find(chunkPos);
    if (old != region.chunks.end()) {
        region.gpu->destroyChunkMesh(old->second);
        region.chunks.erase(old);
    }

    ChunkMesh mesh = region.gpu->createChunkMesh(vertices, indices);

    if (mesh.status == ChunkMeshStatus::OutOfMemory) {
        // Rebuild with enough headroom for the incoming mesh.
        const bool rebuilt = rebuildRegion(
            chunkToRegionPos(chunkPos),
            vertices.size(),
            indices.size()
        );
        if (!rebuilt) {
            std::cerr << "[FATAL] Region rebuild failed permanently\n";
            return;
        }

        mesh = region.gpu->createChunkMesh(vertices, indices);
        if (!mesh.valid) {
            std::cerr << "[FATAL] Region rebuild failed permanently\n";
            return;
        }
    }

    region.chunks.emplace(chunkPos, mesh);
}





void ChunkManager::removeChunkMesh(const glm::ivec3& chunkPos) {
    glm::ivec3 regionPos = chunkToRegionPos(chunkPos);

    auto regionIt = regions.find(regionPos);
    if (regionIt == regions.end()) return;

    Region& region = regionIt->second;
    auto meshIt = region.chunks.find(chunkPos);
    if (meshIt != region.chunks.end()) {
        region.gpu->destroyChunkMesh(meshIt->second);
        region.chunks.erase(meshIt);
    }

    // Optional: Remove empty regions to free GPU memory
    if (region.chunks.empty()) {
        regions.erase(regionIt);
    }
}








bool ChunkManager::rebuildRegion(const glm::ivec3& regionPos, size_t reserveVertices, size_t reserveIndices)
{
    auto it = regions.find(regionPos);
    if (it == regions.end()) return false;

    Region& oldRegion = it->second;

    struct BuiltChunkData {
        glm::ivec3 chunkPos;
        std::vector<VoxelVertex> vertices;
        std::vector<uint16_t> indices;
    };

    std::vector<BuiltChunkData> rebuiltData;
    rebuiltData.reserve(oldRegion.chunks.size());

    size_t requiredVertices = reserveVertices;
    size_t requiredIndices = reserveIndices;

    for (const auto& [chunkPos, oldMesh] : oldRegion.chunks) {
        Chunk& chunk = chunkMap.at(chunkPos);

        auto findChunk = [&](const glm::ivec3& pos) -> const Chunk* {
            auto it = chunkMap.find(pos);
            return (it != chunkMap.end()) ? &it->second : nullptr;
            };

        const Chunk* neighbors[6] = {};
        constexpr glm::ivec3 offsets[6] = {
            {1,0,0},{-1,0,0},
            {0,1,0},{0,-1,0},
            {0,0,1},{0,0,-1}
        };

        for (int i = 0; i < 6; ++i)
            neighbors[i] = findChunk(chunkPos + offsets[i]);

        auto built = builder.buildChunkMesh(
            chunk, neighbors, chunkPos, atlas, enableAO, enableShadows,
            [this](int wx, int wz) { return this->getColumnTopOccluderY(wx, wz); }
        );

        requiredVertices += built.vertices.size();
        requiredIndices += built.indices.size();
        rebuiltData.push_back({ chunkPos, std::move(built.vertices), std::move(built.indices) });
    }

    size_t newVertexBytes = oldRegion.vertexBytes;
    size_t newIndexBytes = oldRegion.indexBytes;

    auto vertexCapacityFromBytes = [](size_t bytes) -> size_t {
        return bytes / sizeof(VoxelVertex);
    };
    auto indexCapacityFromBytes = [](size_t bytes) -> size_t {
        return bytes / sizeof(uint16_t);
    };

    while (vertexCapacityFromBytes(newVertexBytes) < requiredVertices) {
        newVertexBytes *= 2;
    }
    while (indexCapacityFromBytes(newIndexBytes) < requiredIndices) {
        newIndexBytes *= 2;
    }

    if (newVertexBytes != oldRegion.vertexBytes || newIndexBytes != oldRegion.indexBytes) {
        std::cout
            << "[ChunkManager] Growing region (" << regionPos.x << "," << regionPos.y << "," << regionPos.z << ") "
            << "VBO " << oldRegion.vertexBytes << " -> " << newVertexBytes << " bytes, "
            << "EBO " << oldRegion.indexBytes << " -> " << newIndexBytes << " bytes\n";
    }

    auto newGpu = std::make_unique<RegionMeshBuffer>(newVertexBytes, newIndexBytes);
    std::unordered_map<glm::ivec3, ChunkMesh, IVec3Hash> newMeshes;
    newMeshes.reserve(rebuiltData.size());

    for (auto& entry : rebuiltData) {
        ChunkMesh mesh = newGpu->createChunkMesh(entry.vertices, entry.indices);
        if (!mesh.valid) {
            std::cerr << "[FATAL] Region rebuild failed\n";
            return false;
        }
        newMeshes.emplace(entry.chunkPos, mesh);
    }

    oldRegion.vertexBytes = newVertexBytes;
    oldRegion.indexBytes = newIndexBytes;
    oldRegion.gpu = std::move(newGpu);
    oldRegion.chunks = std::move(newMeshes);
    return true;
}




void RegionMeshBuffer::orphanBuffers()
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        vertexCapacity * sizeof(VoxelVertex),
        nullptr,
        GL_DYNAMIC_DRAW
    );

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        indexCapacity * sizeof(uint16_t),
        nullptr,
        GL_DYNAMIC_DRAW
    );
}









ChunkColumn& ChunkManager::getOrCreateColumn(int colX, int colZ) {
    glm::ivec2 pos(colX, colZ);

    auto it = chunkColumns.find(pos);

    if (it != chunkColumns.end()) {
        return it->second;
    }

    ChunkColumn& newCol = chunkColumns[pos];


    newCol.chunkX = colX;
    newCol.chunkZ = colZ;


    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            newCol.sunLitBlocksYvalue[x][z] = -128;
        }
    }


    return newCol;
}

int ChunkManager::getColumnTopOccluderY(int worldX, int worldZ) const {
    const int colX = floorDiv(worldX, CHUNK_SIZE);
    const int colZ = floorDiv(worldZ, CHUNK_SIZE);
    const int lx = mod(worldX, CHUNK_SIZE);
    const int lz = mod(worldZ, CHUNK_SIZE);

    auto it = chunkColumns.find(glm::ivec2(colX, colZ));
    if (it == chunkColumns.end()) {
        return WORLD_MIN_Y - 1;
    }
    return int(it->second.sunLitBlocksYvalue[lx][lz]);
}

void ChunkManager::rebuildColumnSunCache(int colChunkX, int colChunkZ) {
    ChunkColumn& col = getOrCreateColumn(colChunkX, colChunkZ);

    for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
        for (int lz = 0; lz < CHUNK_SIZE; ++lz) {
            const int worldX = colChunkX * CHUNK_SIZE + lx;
            const int worldZ = colChunkZ * CHUNK_SIZE + lz;

            int top = WORLD_MIN_Y - 1;
            for (int y = WORLD_MAX_Y; y >= WORLD_MIN_Y; --y) {
                if (getBlockGlobal(worldX, y, worldZ) != BlockID::Air) {
                    top = y;
                    break;
                }
            }

            col.sunLitBlocksYvalue[lx][lz] = int8_t(top);
        }
    }
}

void ChunkManager::updateColumnSunCacheForBlockChange(int worldX, int worldY, int worldZ, BlockID oldId, BlockID newId) {
    const int colX = floorDiv(worldX, CHUNK_SIZE);
    const int colZ = floorDiv(worldZ, CHUNK_SIZE);
    const int lx = mod(worldX, CHUNK_SIZE);
    const int lz = mod(worldZ, CHUNK_SIZE);

    ChunkColumn& col = getOrCreateColumn(colX, colZ);
    const int oldTop = int(col.sunLitBlocksYvalue[lx][lz]);
    int newTop = oldTop;
    const auto rebuildAffectedColumns = [&](int oldTopY, int newTopY) {
        rebuildSunlightAffectedColumnChunks(colX, colZ, oldTopY, newTopY);

        const bool minX = (lx == 0);
        const bool maxX = (lx == CHUNK_SIZE - 1);
        const bool minZ = (lz == 0);
        const bool maxZ = (lz == CHUNK_SIZE - 1);

        if (minX) rebuildSunlightAffectedColumnChunks(colX - 1, colZ, oldTopY, newTopY);
        if (maxX) rebuildSunlightAffectedColumnChunks(colX + 1, colZ, oldTopY, newTopY);
        if (minZ) rebuildSunlightAffectedColumnChunks(colX, colZ - 1, oldTopY, newTopY);
        if (maxZ) rebuildSunlightAffectedColumnChunks(colX, colZ + 1, oldTopY, newTopY);

        if (minX && minZ) rebuildSunlightAffectedColumnChunks(colX - 1, colZ - 1, oldTopY, newTopY);
        if (minX && maxZ) rebuildSunlightAffectedColumnChunks(colX - 1, colZ + 1, oldTopY, newTopY);
        if (maxX && minZ) rebuildSunlightAffectedColumnChunks(colX + 1, colZ - 1, oldTopY, newTopY);
        if (maxX && maxZ) rebuildSunlightAffectedColumnChunks(colX + 1, colZ + 1, oldTopY, newTopY);
    };

    if (newId != BlockID::Air) {
        if (worldY > oldTop) {
            newTop = worldY;
            col.sunLitBlocksYvalue[lx][lz] = int8_t(newTop);
            if (!suppressSunlightAffectedRebuilds) {
                rebuildAffectedColumns(oldTop, newTop);
            }
        }
        return;
    }

    if (oldId == BlockID::Air) {
        return;
    }

    if (worldY == oldTop) {
        newTop = WORLD_MIN_Y - 1;
        for (int y = worldY - 1; y >= WORLD_MIN_Y; --y) {
            if (getBlockGlobal(worldX, y, worldZ) != BlockID::Air) {
                newTop = y;
                break;
            }
        }
        col.sunLitBlocksYvalue[lx][lz] = int8_t(newTop);
        if (!suppressSunlightAffectedRebuilds) {
            rebuildAffectedColumns(oldTop, newTop);
        }
    }
}

void ChunkManager::rebuildSunlightAffectedColumnChunks(int colChunkX, int colChunkZ, int oldTopY, int newTopY) {
    if (oldTopY == newTopY) {
        return;
    }

    const int minChunkY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxAffectedY = (oldTopY > newTopY) ? oldTopY : newTopY;
    const int maxChunkY = floorDiv(maxAffectedY, CHUNK_SIZE);

    for (int chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY) {
        updateDirtyChunkAt(glm::ivec3(colChunkX, chunkY, colChunkZ));
    }
}



