#include <windows.h>
#include <psapi.h>

struct ProcessMemoryStatsMB {
    size_t privateMB = 0;
    size_t workingSetMB = 0;
};

static ProcessMemoryStatsMB getProcessMemoryMB() {
    ProcessMemoryStatsMB stats;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        stats.privateMB = size_t(pmc.PrivateUsage / (1024 * 1024));
        stats.workingSetMB = size_t(pmc.WorkingSetSize / (1024 * 1024));
    }
    return stats;
}

#include "ChunkManager.hpp"
#include "../render/meshing/ChunkMesher.hpp"
#include "../network/DecompressChunk.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <iostream>
#include <unordered_set>
#include <vector>

namespace {
    uint32_t fnv1a32(const uint8_t *data, size_t size) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < size; ++i) {
            h ^= static_cast<uint32_t>(data[i]);
            h *= 16777619u;
        }
        return h;
    }
} // namespace

constexpr bool kEnableMissingChunkUnloadLogs = false;
constexpr bool kTraceChunkLifecycle = false;

ChunkManager::ChunkManager() {
    m_chunkMesher = std::make_unique<ChunkMesher>(*this);

    // build the chunk storage with positions set using unordered_map keyed by glm::ivec3
    // chunkMap.clear();
}

ChunkManager::~ChunkManager() = default;

void ChunkManager::markChunkDirty(const glm::ivec3 &pos) {
    m_chunkMesher->markChunkDirty(pos);
}

void ChunkManager::markChunkDirtyHighPriority(const glm::ivec3 &pos) {
    m_chunkMesher->markChunkDirtyHighPriority(pos);
}

void ChunkManager::rebuildAllChunkMeshes(bool highPriority) {
    size_t queued = 0;
    for (const auto &entry : chunkMap) {
        const glm::ivec3 &chunkPos = entry.first;
        if (highPriority) {
            markChunkDirtyHighPriority(chunkPos);
        } else {
            markChunkDirty(chunkPos);
        }
        ++queued;
    }
    std::cout << "[chunk] queued full mesh rebuild for " << queued
              << " chunks (" << (highPriority ? "high-priority" : "normal") << ").\n";
}

void ChunkManager::updateDirtyChunks(size_t maxChunksPerCall, int64_t maxBudgetUs) {
    m_chunkMesher->updateDirtyChunks(maxChunksPerCall, maxBudgetUs);
}

bool ChunkManager::applyNetworkChunkData(const ChunkData &packet) {
    std::vector<uint8_t> decodedPayload;
    if (!DecompressChunkPayload(packet.flags, packet.payload, decodedPayload)) {
        std::cerr << "[chunk/apply] failed to decode payload flags="
                  << static_cast<int>(packet.flags) << " chunk=(" << packet.chunkX << ","
                  << packet.chunkY << "," << packet.chunkZ << ")"
                  << " payloadBytes=" << packet.payload.size() << "\n";
        return false;
    }

    const size_t rawBlockBytes = CHUNK_VOLUME * sizeof(BlockID);
    if (decodedPayload.size() != rawBlockBytes) {
        std::cerr << "[chunk/apply] invalid ChunkData payload size=" << decodedPayload.size()
                  << " expected=" << rawBlockBytes << " chunk=(" << packet.chunkX << ","
                  << packet.chunkY << "," << packet.chunkZ << ")\n";
        return false;
    }

    const uint32_t rawContentHash = fnv1a32(decodedPayload.data(), decodedPayload.size());
    const uint8_t *raw = decodedPayload.data();
    glm::ivec3 chunkPos(packet.chunkX, packet.chunkY, packet.chunkZ);
    const uint64_t incomingVersion = packet.version;
    const bool hadChunkBeforeApply = chunkMap.find(chunkPos) != chunkMap.end();

    if (kTraceChunkLifecycle) {
        std::cerr << "[chunk/apply] data begin chunk=(" << chunkPos.x << "," << chunkPos.y << ","
                  << chunkPos.z << ") version=" << incomingVersion
                  << " hadChunk=" << (hadChunkBeforeApply ? 1 : 0)
                  << " payloadHash=" << rawContentHash << "\n";
    }

    auto knownVersionIt = m_networkChunkVersions.find(chunkPos);
    auto existingChunkIt = chunkMap.find(chunkPos);
    const bool chunkLoaded = existingChunkIt != chunkMap.end();
    if (knownVersionIt != m_networkChunkVersions.end() && chunkLoaded) {
        bool samePayload = true;
        Chunk &existingChunk = existingChunkIt->second;
        for (int z = 0; samePayload && z < CHUNK_SIZE; ++z) {
            for (int y = 0; samePayload && y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    const size_t i = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_SIZE * z));
                    if (existingChunk.getBlock(x, y, z) != static_cast<BlockID>(raw[i])) {
                        samePayload = false;
                        break;
                    }
                }
            }
        }

        if (samePayload) {
            const auto &cpuMeshes = m_chunkMesher->getCpuChunkMeshes();
            const bool hasMesh = cpuMeshes.find(chunkPos) != cpuMeshes.end();
            if (!existingChunkIt->second.isCompletelyAir() && !hasMesh) {
                markChunkDirtyHighPriority(chunkPos);
                if (kTraceChunkLifecycle) {
                    std::cerr << "[chunk/apply] data same-payload remesh chunk=(" << chunkPos.x
                              << "," << chunkPos.y << "," << chunkPos.z << ") version="
                              << incomingVersion << "\n";
                }
                return true;
            }

            if (kTraceChunkLifecycle) {
                std::cerr << "[chunk/apply] data same-payload ok chunk=(" << chunkPos.x << ","
                          << chunkPos.y << "," << chunkPos.z << ") version=" << incomingVersion
                          << "\n";
            }
            return true;
        }
    }

    m_chunkMesher->onChunkRemoved(chunkPos);
    chunkMap.erase(chunkPos);
    auto [chunkIt, inserted] = chunkMap.try_emplace(chunkPos, chunkPos);
    (void)inserted;

    Chunk &chunk = chunkIt->second;
    size_t nonAirCount = 0;
    int minNonAirLocalY = CHUNK_SIZE;
    int maxNonAirLocalY = -1;
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                const size_t i = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_SIZE * z));
                const BlockID id = static_cast<BlockID>(raw[i]);
                if (id != BlockID::Air) {
                    chunk.setBlock(x, y, z, id);
                    ++nonAirCount;
                    minNonAirLocalY = std::min(minNonAirLocalY, y);
                    maxNonAirLocalY = std::max(maxNonAirLocalY, y);
                }
            }
        }
    }

    const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    if (chunkPos.y == minChunkY && nonAirCount < static_cast<size_t>(CHUNK_SIZE * CHUNK_SIZE)) {
        std::cerr << "[chunk/apply] suspicious low nonAir in bottom chunk chunk=(" << chunkPos.x
                  << "," << chunkPos.y << "," << chunkPos.z << ")"
                  << " nonAir=" << nonAirCount << " rawContentHash=" << rawContentHash
                  << " payloadBytes=" << decodedPayload.size() << "\n";
    }

    markChunkDirty(chunkPos);

    static const glm::ivec3 dirs[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    for (const glm::ivec3 &d : dirs) {
        const glm::ivec3 n = chunkPos + d;
        if (chunkMap.find(n) != chunkMap.end()) {
            markChunkDirty(n);
        }
    }

    m_networkChunkVersions[chunkPos] = incomingVersion;
    NetworkChunkDebugSummary summary{};
    summary.version = incomingVersion;
    summary.rawContentHash = rawContentHash;
    summary.nonAirCount = static_cast<uint16_t>(std::min<size_t>(nonAirCount, UINT16_MAX));
    if (nonAirCount > 0) {
        summary.minNonAirWorldY = chunkPos.y * CHUNK_SIZE + minNonAirLocalY;
        summary.maxNonAirWorldY = chunkPos.y * CHUNK_SIZE + maxNonAirLocalY;
    } else {
        summary.minNonAirWorldY = chunkPos.y * CHUNK_SIZE;
        summary.maxNonAirWorldY = chunkPos.y * CHUNK_SIZE - 1;
    }
    m_networkChunkDebugSummaries[chunkPos] = summary;
    if (kTraceChunkLifecycle) {
        std::cerr << "[chunk/apply] data inserted chunk=(" << chunkPos.x << "," << chunkPos.y
                  << "," << chunkPos.z << ") version=" << incomingVersion
                  << " nonAir=" << nonAirCount << " chunkMapSize=" << chunkMap.size() << "\n";
    }
    return true;
}

NetworkChunkDeltaApplyResult ChunkManager::applyNetworkChunkDelta(const ChunkDelta &packet) {
    const glm::ivec3 chunkPos(packet.chunkX, packet.chunkY, packet.chunkZ);
    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end()) {
        return NetworkChunkDeltaApplyResult::MissingBaseChunk;
    }

    const auto versionIt = m_networkChunkVersions.find(chunkPos);
    if (versionIt == m_networkChunkVersions.end()) {
        return NetworkChunkDeltaApplyResult::MissingBaseChunk;
    }

    const uint64_t knownVersion = versionIt->second;
    const uint64_t incomingVersion = packet.resultingVersion;
    if (incomingVersion <= knownVersion) {
        return NetworkChunkDeltaApplyResult::StaleVersion;
    }

    Chunk &chunk = it->second;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> rebuildSet;
    rebuildSet.insert(chunkPos);

    for (const ChunkDeltaOp &op : packet.edits) {
        if (!Chunk::inBounds(
                static_cast<int>(op.x), static_cast<int>(op.y), static_cast<int>(op.z)
            )) {
            continue;
        }

        const BlockID newId = static_cast<BlockID>(op.blockId);
        const BlockID oldId =
            chunk.getBlock(static_cast<int>(op.x), static_cast<int>(op.y), static_cast<int>(op.z));
        if (oldId == newId) {
            continue;
        }

        chunk.setBlock(
            static_cast<int>(op.x), static_cast<int>(op.y), static_cast<int>(op.z), newId
        );

        if (op.x == 0)
            rebuildSet.insert(chunkPos + glm::ivec3(-1, 0, 0));
        if (op.x == CHUNK_SIZE - 1)
            rebuildSet.insert(chunkPos + glm::ivec3(1, 0, 0));
        if (op.y == 0)
            rebuildSet.insert(chunkPos + glm::ivec3(0, -1, 0));
        if (op.y == CHUNK_SIZE - 1)
            rebuildSet.insert(chunkPos + glm::ivec3(0, 1, 0));
        if (op.z == 0)
            rebuildSet.insert(chunkPos + glm::ivec3(0, 0, -1));
        if (op.z == CHUNK_SIZE - 1)
            rebuildSet.insert(chunkPos + glm::ivec3(0, 0, 1));
    }

    for (const glm::ivec3 &pos : rebuildSet) {
        if (chunkMap.find(pos) != chunkMap.end()) {
            markChunkDirtyHighPriority(pos);
        }
    }

    m_networkChunkVersions[chunkPos] = incomingVersion;
    return NetworkChunkDeltaApplyResult::Applied;
}

void ChunkManager::clearNetworkChunks() {
    for (const auto &[chunkPos, _] : chunkMap) {
        m_chunkMesher->onChunkRemoved(chunkPos);
    }
    chunkMap.clear();
    m_networkChunkVersions.clear();
    m_networkChunkDebugSummaries.clear();
}

void ChunkManager::applyNetworkChunkUnload(const ChunkUnload &packet) {
    const glm::ivec3 chunkPos(packet.chunkX, packet.chunkY, packet.chunkZ);
    auto it = chunkMap.find(chunkPos);
    if (kTraceChunkLifecycle) {
        std::cerr << "[chunk/apply] unload chunk=(" << chunkPos.x << "," << chunkPos.y << ","
                  << chunkPos.z << ") existed=" << (it != chunkMap.end() ? 1 : 0)
                  << " chunkMapSizeBefore=" << chunkMap.size() << "\n";
    }
    if (it == chunkMap.end()) {
        m_networkChunkVersions.erase(chunkPos);
        m_networkChunkDebugSummaries.erase(chunkPos);
        m_chunkMesher->onChunkRemoved(chunkPos);
        if (kEnableMissingChunkUnloadLogs) {
            static uint64_t missingChunkUnloadCount = 0;
            ++missingChunkUnloadCount;
            if (missingChunkUnloadCount <= 20 || (missingChunkUnloadCount % 100) == 0) {
                std::cerr << "[chunk/unload] unload for missing chunk=(" << packet.chunkX << ","
                          << packet.chunkY << "," << packet.chunkZ
                          << ") count=" << missingChunkUnloadCount << "\n";
            }
        }
        return;
    }

    chunkMap.erase(it);
    m_networkChunkVersions.erase(chunkPos);
    m_networkChunkDebugSummaries.erase(chunkPos);
    m_chunkMesher->onChunkRemoved(chunkPos);

    if (kTraceChunkLifecycle) {
        std::cerr << "[chunk/apply] unload removed chunk=(" << chunkPos.x << "," << chunkPos.y
                  << "," << chunkPos.z << ") chunkMapSizeAfter=" << chunkMap.size() << "\n";
    }

    static const glm::ivec3 dirs[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    for (const glm::ivec3 &d : dirs) {
        const glm::ivec3 n = chunkPos + d;
        if (chunkMap.find(n) != chunkMap.end()) {
            markChunkDirty(n);
        }
    }
}

void ChunkManager::setBlockInWorld(const glm::ivec3 &worldPos, BlockID blockID) {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    if (!inBounds(chunkPos))
        return;

    auto it = chunkMap.find(chunkPos);
    if (it == chunkMap.end())
        return;

    Chunk &chunk = it->second;
    BlockID oldId = chunk.getBlock(localPos.x, localPos.y, localPos.z);
    if (oldId == blockID)
        return;
    chunk.setBlock(localPos.x, localPos.y, localPos.z, blockID);
    markChunkDirty(chunkPos);

    // mark neighbors if we touched an edge
    if (localPos.x == 0)
        markChunkDirty(chunkPos + glm::ivec3(-1, 0, 0));
    if (localPos.x == CHUNK_SIZE - 1)
        markChunkDirty(chunkPos + glm::ivec3(1, 0, 0));
    if (localPos.y == 0)
        markChunkDirty(chunkPos + glm::ivec3(0, -1, 0));
    if (localPos.y == CHUNK_SIZE - 1)
        markChunkDirty(chunkPos + glm::ivec3(0, 1, 0));
    if (localPos.z == 0)
        markChunkDirty(chunkPos + glm::ivec3(0, 0, -1));
    if (localPos.z == CHUNK_SIZE - 1)
        markChunkDirty(chunkPos + glm::ivec3(0, 0, 1));
}

glm::ivec3 ChunkManager::worldToChunkPos(const glm::ivec3 &worldPos) const {
    // floor division to get the chunk indices (works for negatives)
    glm::vec3 f = glm::floor(glm::vec3(worldPos) / float(CHUNK_SIZE));
    return glm::ivec3(static_cast<int>(f.x), static_cast<int>(f.y), static_cast<int>(f.z));
}

bool ChunkManager::hasChunkLoaded(const glm::ivec3 &chunkPos) const {
    return chunkMap.find(chunkPos) != chunkMap.end();
}

std::optional<NetworkChunkDebugSummary>
ChunkManager::getNetworkChunkDebugSummary(const glm::ivec3 &chunkPos) const {
    const auto it = m_networkChunkDebugSummaries.find(chunkPos);
    if (it == m_networkChunkDebugSummaries.end()) {
        return std::nullopt;
    }
    return it->second;
}

glm::ivec3 ChunkManager::worldToLocalPos(const glm::ivec3 &worldPos) const {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 local = worldPos - chunkPos * CHUNK_SIZE;
    return local;
}

bool ChunkManager::inBounds(const glm::ivec3 &pos) const {
    const int minChunkY = floorDiv(WORLD_MIN_Y, CHUNK_SIZE);
    const int maxChunkY = floorDiv(WORLD_MAX_Y, CHUNK_SIZE);
    return pos.x >= WORLD_MIN_X && pos.x <= WORLD_MAX_X && pos.y >= minChunkY &&
           pos.y <= maxChunkY && pos.z >= WORLD_MIN_Z && pos.z <= WORLD_MAX_Z;
}

void ChunkManager::setBlockGlobal(int worldX, int worldY, int worldZ, BlockID id) {
    glm::ivec3 worldPos(worldX, worldY, worldZ);
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    glm::ivec3 localPos = worldToLocalPos(worldPos);

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        BlockID oldId = it->second.getBlock(localPos.x, localPos.y, localPos.z);
        if (oldId == id)
            return;
        it->second.setBlock(localPos.x, localPos.y, localPos.z, id);
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

void ChunkManager::setBlockSafe(Chunk &currentChunk, const glm::ivec3 &pos, BlockID id) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE && pos.y >= 0 && pos.y < CHUNK_SIZE && pos.z >= 0 &&
        pos.z < CHUNK_SIZE) {
        BlockID oldId = currentChunk.getBlock(pos.x, pos.y, pos.z);
        if (oldId == id)
            return;
        currentChunk.setBlock(pos.x, pos.y, pos.z, id);
    } else {
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        setBlockGlobal(worldPos.x, worldPos.y, worldPos.z, id);
    }
}

BlockID ChunkManager::getBlockSafe(Chunk &currentChunk, const glm::ivec3 &pos) {
    if (pos.x >= 0 && pos.x < CHUNK_SIZE && pos.y >= 0 && pos.y < CHUNK_SIZE && pos.z >= 0 &&
        pos.z < CHUNK_SIZE) {
        return currentChunk.getBlock(pos.x, pos.y, pos.z);
    } else {
        glm::ivec3 worldPos = currentChunk.getWorldPosition() + pos;
        return getBlockGlobal(worldPos.x, worldPos.y, worldPos.z);
    }
}

void ChunkManager::debugMemoryEstimate() {
    std::cout << "---- MEMORY ESTIMATE ----\n";

    const ProcessMemoryStatsMB mem = getProcessMemoryMB();
    std::cout << "Process private bytes (MB): " << mem.privateMB << "\n";
    std::cout << "Process working set (MB): " << mem.workingSetMB << "\n";

    std::cout << "sizeof(Chunk): " << sizeof(Chunk) << " bytes\n";

    std::cout << "chunkMap.size(): " << chunkMap.size() << "\n";

    double chunkMB = chunkMap.size() * sizeof(Chunk) / (1024.0 * 1024.0);

    std::cout << "estimated raw chunk bytes: " << chunkMB << " MB\n";

    const MeshBuildProfileSnapshot p = ChunkMeshBuilder::getProfileSnapshot();
    if (p.chunksMeshed > 0 && p.totalUs > 0) {
        const double invChunks = 1.0 / double(p.chunksMeshed);
        const double avgTotal = double(p.totalUs) * invChunks;
        const auto pct = [&](uint64_t us) { return (100.0 * double(us)) / double(p.totalUs); };
        const uint64_t profiledUs = p.blockGridUs + p.solidCacheUs + p.sunlightPrepUs + p.aoPrepUs +
                                    p.maskTransitionUs + p.maskLightingUs + p.greedyEmitUs;
        const uint64_t otherUs = (p.totalUs > profiledUs) ? (p.totalUs - profiledUs) : 0;
        std::cout << "Mesher profile (" << p.chunksMeshed << " chunks):\n";
        std::cout << "  avg total: " << avgTotal << " us/chunk\n";
        std::cout << "  block grid: " << (double(p.blockGridUs) * invChunks) << " us ("
                  << pct(p.blockGridUs) << "%)\n";
        std::cout << "  solid cache: " << (double(p.solidCacheUs) * invChunks) << " us ("
                  << pct(p.solidCacheUs) << "%)\n";
        std::cout << "  sunlight prep: " << (double(p.sunlightPrepUs) * invChunks) << " us ("
                  << pct(p.sunlightPrepUs) << "%)\n";
        std::cout << "  AO prep: " << (double(p.aoPrepUs) * invChunks) << " us (" << pct(p.aoPrepUs)
                  << "%)\n";
        std::cout << "  mask transitions: " << (double(p.maskTransitionUs) * invChunks) << " us ("
                  << pct(p.maskTransitionUs) << "%)\n";
        std::cout << "  mask lighting: " << (double(p.maskLightingUs) * invChunks) << " us ("
                  << pct(p.maskLightingUs) << "%)\n";
        std::cout << "  mask build: " << (double(p.maskBuildUs) * invChunks) << " us ("
                  << pct(p.maskBuildUs) << "%)\n";
        std::cout << "  greedy emit: " << (double(p.greedyEmitUs) * invChunks) << " us ("
                  << pct(p.greedyEmitUs) << "%)\n";
        std::cout << "  other/unprofiled: " << (double(otherUs) * invChunks) << " us ("
                  << pct(otherUs) << "%)\n";
    }
}

void ChunkManager::playerBreakBlockAt(const glm::ivec3 &blockCoords) {
    glm::ivec3 chunkPos = worldToChunkPos(blockCoords);
    glm::ivec3 localPos = worldToLocalPos(blockCoords);
    bool changed = false;

    auto it = chunkMap.find(chunkPos);
    if (it != chunkMap.end()) {
        BlockID oldId = it->second.removeBlock(localPos.x, localPos.y, localPos.z);
        if (oldId != BlockID::Air) {
            changed = true;
        }
    }

    if (!changed)
        return;

    markChunkDirtyHighPriority(chunkPos);

    // order matches isEdgeBlock: { x==0, x==15, y==0, y==15, z==0, z==15 }
    const std::array<glm::ivec3, 6> neighborOffsets = {
        glm::ivec3(-1, 0, 0), // x==0 -> neighbor x-1
        glm::ivec3(+1, 0, 0), // x==15 -> neighbor x+1
        glm::ivec3(0, -1, 0), // y==0 -> y-1
        glm::ivec3(0, +1, 0), // y==15 -> y+1
        glm::ivec3(0, 0, -1), // z==0 -> z-1
        glm::ivec3(0, 0, +1)  // z==15 -> z+1
    };

    auto edges = isEdgeBlock(localPos);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!edges[i])
            continue;
        glm::ivec3 neighborChunk = chunkPos + neighborOffsets[i];

        if (chunkMap.find(neighborChunk) != chunkMap.end()) {
            markChunkDirtyHighPriority(neighborChunk);
        }
    }
}

void ChunkManager::playerPlaceBlockAt(glm::ivec3 blockCoords, int faceNormal, BlockID blockType) {
    (void)faceNormal;
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToRebuild;

    const auto queueChunkAndEdgeNeighbors = [&](const glm::ivec3 &worldPos) {
        const glm::ivec3 chunkPos = worldToChunkPos(worldPos);
        const glm::ivec3 localPos = worldToLocalPos(worldPos);

        chunksToRebuild.insert(chunkPos);
        if (localPos.x == 0)
            chunksToRebuild.insert(chunkPos + glm::ivec3(-1, 0, 0));
        if (localPos.x == CHUNK_SIZE - 1)
            chunksToRebuild.insert(chunkPos + glm::ivec3(1, 0, 0));
        if (localPos.y == 0)
            chunksToRebuild.insert(chunkPos + glm::ivec3(0, -1, 0));
        if (localPos.y == CHUNK_SIZE - 1)
            chunksToRebuild.insert(chunkPos + glm::ivec3(0, 1, 0));
        if (localPos.z == 0)
            chunksToRebuild.insert(chunkPos + glm::ivec3(0, 0, -1));
        if (localPos.z == CHUNK_SIZE - 1)
            chunksToRebuild.insert(chunkPos + glm::ivec3(0, 0, 1));
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

    for (const auto &pos : chunksToRebuild) {
        markChunkDirtyHighPriority(pos);
    }
}

void ChunkManager::updateDirtyChunkAt(const glm::ivec3 &chunkPos) {
    m_chunkMesher->updateDirtyChunkAt(chunkPos);
}

const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &
ChunkManager::getCpuChunkMeshesImpl() const noexcept {
    return m_chunkMesher->getCpuChunkMeshes();
}

uint64_t ChunkManager::getCpuChunkMeshesVersion() const noexcept {
    return getCpuChunkMeshesVersionImpl();
}

uint64_t ChunkManager::getCpuChunkMeshesVersionImpl() const noexcept {
    return m_chunkMesher->getCpuChunkMeshesVersion();
}

