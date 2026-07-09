#include "ChunkStreamingClient.hpp"

#include "../application/AppHelpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_set>
#include <vector>

using namespace AppHelpers;

namespace {

    void MarkChunkAndEdgeNeighborsDirty(ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
        const glm::ivec3 localPos = chunkManager.worldToLocalPos(worldPos);
        chunkManager.markChunkDirtyHighPriority(chunkPos);
        if (localPos.x == 0) {
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(-1, 0, 0));
        }
        if (localPos.x == CHUNK_SIZE - 1) {
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(1, 0, 0));
        }
        if (localPos.y == 0) {
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, -1, 0));
        }
        if (localPos.y == CHUNK_SIZE - 1) {
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, 1, 0));
        }
        if (localPos.z == 0) {
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, 0, -1));
        }
        if (localPos.z == CHUNK_SIZE - 1) {
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, 0, 1));
        }
    }

} // namespace

void ChunkStreamingClient::update(Runtime &runtime, bool prioritizeMovement) {
    constexpr double kChunkResyncCooldownSec = 2.0;
    constexpr double kChunkResyncGlobalIntervalSec = 0.25;

    const auto requestChunkResync = [&](const glm::ivec3 &chunkPos, bool force) {
        const double nowSec = GetTimeSeconds();
        if (nowSec < m_nextChunkResyncSendAt) {
            return;
        }
        auto it = m_chunkResyncCooldownUntil.find(chunkPos);
        if (!force && it != m_chunkResyncCooldownUntil.end() && nowSec < it->second) {
            return;
        }
        if (force && it != m_chunkResyncCooldownUntil.end() && nowSec < it->second) {
            return;
        }
        m_nextChunkResyncSendAt = nowSec + kChunkResyncGlobalIntervalSec;
        m_chunkResyncCooldownUntil[chunkPos] = nowSec + kChunkResyncCooldownSec;
        if (!runtime.network.clientNet.SendChunkResyncRequest(chunkPos)) {
            std::cerr << "[chunk/resync] failed to request full chunk (" << chunkPos.x << ","
                      << chunkPos.y << "," << chunkPos.z << ")\n";
        }
    };

    const ClientNetwork::ChunkQueueDepths initialQueueDepths =
        runtime.network.clientNet.GetChunkQueueDepths();
    const bool chunkDataBacklog = initialQueueDepths.chunkData > 256;
    const int64_t chunkApplyBudgetUs =
        chunkDataBacklog
            ? (prioritizeMovement ? RuntimeWorldState::ChunkApplyBudgetUsUnderInputPressure * 2
                                  : RuntimeWorldState::ChunkApplyBudgetUs * 2)
            : (prioritizeMovement ? RuntimeWorldState::ChunkApplyBudgetUsUnderInputPressure
                                  : RuntimeWorldState::ChunkApplyBudgetUs);
    const size_t maxChunkDataApplyThisFrame =
        chunkDataBacklog ? (prioritizeMovement ? RuntimeWorldState::MaxChunkDataApplyPerFrame
                                               : RuntimeWorldState::MaxChunkDataApplyPerFrame * 3)
                         : RuntimeWorldState::MaxChunkDataApplyPerFrame;
    const auto chunkApplyStart = std::chrono::steady_clock::now();
    const auto withinChunkApplyBudget = [&]() -> bool {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - chunkApplyStart
        )
                                   .count();
        return elapsedUs < chunkApplyBudgetUs;
    };
    size_t chunkDataApplied = 0;

    ChunkData chunkData;
    while (chunkDataApplied < maxChunkDataApplyThisFrame && withinChunkApplyBudget() &&
           runtime.network.clientNet.PopChunkData(chunkData)) {
        runtime.gameplay.chunkManager->applyNetworkChunkData(chunkData);
        ++chunkDataApplied;
    }

    ChunkDelta chunkDelta;
    size_t chunkDeltaApplied = 0;
    while (chunkDeltaApplied < RuntimeWorldState::MaxChunkDeltaApplyPerFrame &&
           withinChunkApplyBudget() && runtime.network.clientNet.PopChunkDelta(chunkDelta)) {
        const NetworkChunkDeltaApplyResult deltaResult =
            runtime.gameplay.chunkManager->applyNetworkChunkDelta(chunkDelta);
        if (deltaResult == NetworkChunkDeltaApplyResult::MissingBaseChunk ||
            deltaResult == NetworkChunkDeltaApplyResult::VersionGap) {
            requestChunkResync(
                glm::ivec3(chunkDelta.chunkX, chunkDelta.chunkY, chunkDelta.chunkZ), false
            );
        }
        ++chunkDeltaApplied;
    }

    ChunkUnload chunkUnload;
    size_t chunkUnloadApplied = 0;
    while (chunkUnloadApplied < RuntimeWorldState::MaxChunkUnloadApplyPerFrame &&
           withinChunkApplyBudget() && runtime.network.clientNet.PopChunkUnload(chunkUnload)) {
        runtime.gameplay.chunkManager->applyNetworkChunkUnload(chunkUnload);
        ++chunkUnloadApplied;
    }

    BlockPlaceResult blockPlaceResult;
    size_t blockPlaceResultsApplied = 0;
    while (blockPlaceResultsApplied < RuntimeWorldState::MaxBlockPlaceResultsPerFrame &&
           runtime.network.clientNet.PopBlockPlaceResult(blockPlaceResult)) {
        auto pendingIt = runtime.world.pendingBlockPlaceRequests.find(blockPlaceResult.requestId);
        if (blockPlaceResult.accepted == 0) {
            if (pendingIt != runtime.world.pendingBlockPlaceRequests.end()) {
                for (const RuntimeWorldState::PendingBlockPlaceEdit &edit :
                     pendingIt->second.edits) {
                    const BlockID predictedId = static_cast<BlockID>(edit.newBlockId);
                    const BlockID rollbackId = static_cast<BlockID>(edit.oldBlockId);
                    if (runtime.gameplay.chunkManager->getBlockGlobal(
                            edit.worldPos.x, edit.worldPos.y, edit.worldPos.z
                        ) == predictedId) {
                        runtime.gameplay.chunkManager->setBlockGlobal(
                            edit.worldPos.x, edit.worldPos.y, edit.worldPos.z, rollbackId
                        );
                        MarkChunkAndEdgeNeighborsDirty(
                            *runtime.gameplay.chunkManager, edit.worldPos
                        );
                    }
                }
            }

            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToResync;
            if (pendingIt != runtime.world.pendingBlockPlaceRequests.end()) {
                for (const glm::ivec3 &chunkPos : pendingIt->second.affectedChunks) {
                    chunksToResync.insert(chunkPos);
                }
            } else {
                for (const BlockPlaceChunkCoord &coord : blockPlaceResult.correctiveChunks) {
                    chunksToResync.insert(glm::ivec3(coord.chunkX, coord.chunkY, coord.chunkZ));
                }
            }

            for (const glm::ivec3 &chunkPos : chunksToResync) {
                requestChunkResync(chunkPos, true);
            }
        }

        if (pendingIt != runtime.world.pendingBlockPlaceRequests.end()) {
            runtime.world.pendingBlockPlaceRequests.erase(pendingIt);
        }
        ++blockPlaceResultsApplied;
    }

    const double nowSec = GetTimeSeconds();
    for (auto it = runtime.world.pendingBlockPlaceRequests.begin();
         it != runtime.world.pendingBlockPlaceRequests.end();) {
        if ((nowSec - it->second.createdAt) > 1.5) {
            for (const glm::ivec3 &chunkPos : it->second.affectedChunks) {
                requestChunkResync(chunkPos, true);
            }
            it = runtime.world.pendingBlockPlaceRequests.erase(it);
        } else {
            ++it;
        }
    }

    BlockBreakResult blockBreakResult;
    size_t blockBreakResultsApplied = 0;
    while (blockBreakResultsApplied < RuntimeWorldState::MaxBlockBreakResultsPerFrame &&
           runtime.network.clientNet.PopBlockBreakResult(blockBreakResult)) {
        auto pendingIt = runtime.world.pendingBlockBreakRequests.find(blockBreakResult.requestId);
        if (blockBreakResult.accepted == 0) {
            if (pendingIt != runtime.world.pendingBlockBreakRequests.end()) {
                for (const RuntimeWorldState::PendingBlockBreakEdit &edit :
                     pendingIt->second.edits) {
                    if (runtime.gameplay.chunkManager->getBlockGlobal(
                            edit.worldPos.x, edit.worldPos.y, edit.worldPos.z
                        ) == BlockID::Air) {
                        runtime.gameplay.chunkManager->setBlockGlobal(
                            edit.worldPos.x,
                            edit.worldPos.y,
                            edit.worldPos.z,
                            static_cast<BlockID>(edit.oldBlockId)
                        );
                        MarkChunkAndEdgeNeighborsDirty(
                            *runtime.gameplay.chunkManager, edit.worldPos
                        );
                    }
                }
            }

            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToResync;
            if (pendingIt != runtime.world.pendingBlockBreakRequests.end()) {
                for (const glm::ivec3 &chunkPos : pendingIt->second.affectedChunks) {
                    chunksToResync.insert(chunkPos);
                }
            } else {
                for (const BlockBreakChunkCoord &coord : blockBreakResult.correctiveChunks) {
                    chunksToResync.insert(glm::ivec3(coord.chunkX, coord.chunkY, coord.chunkZ));
                }
            }

            for (const glm::ivec3 &chunkPos : chunksToResync) {
                requestChunkResync(chunkPos, true);
            }
        }

        if (pendingIt != runtime.world.pendingBlockBreakRequests.end()) {
            runtime.world.pendingBlockBreakRequests.erase(pendingIt);
        }
        ++blockBreakResultsApplied;
    }

    for (auto it = runtime.world.pendingBlockBreakRequests.begin();
         it != runtime.world.pendingBlockBreakRequests.end();) {
        if ((nowSec - it->second.createdAt) > 1.5) {
            for (const glm::ivec3 &chunkPos : it->second.affectedChunks) {
                requestChunkResync(chunkPos, true);
            }
            it = runtime.world.pendingBlockBreakRequests.erase(it);
        } else {
            ++it;
        }
    }

    const size_t maxChunkMeshBuilds =
        prioritizeMovement ? RuntimeWorldState::MaxChunkMeshBuildsPerFrameUnderInputPressure
                           : RuntimeWorldState::MaxChunkMeshBuildsPerFrame;
    const int64_t chunkMeshBuildBudgetUs =
        prioritizeMovement ? RuntimeWorldState::ChunkMeshBuildBudgetUsUnderInputPressure
                           : RuntimeWorldState::ChunkMeshBuildBudgetUs;
    runtime.gameplay.chunkManager->updateDirtyChunks(maxChunkMeshBuilds, chunkMeshBuildBudgetUs);

    const double now = GetTimeSeconds();
    if (kEnableChunkDiagnostics && now - runtime.world.lastChunkCoverageLogTime >= 1.0) {
        runtime.world.lastChunkCoverageLogTime = now;
        const ClientNetwork::ChunkQueueDepths queueDepths =
            runtime.network.clientNet.GetChunkQueueDepths();

        const glm::vec3 pos = runtime.gameplay.player->getPosition();
        const glm::ivec3 worldPos(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
        const glm::ivec3 centerChunk = runtime.gameplay.chunkManager->worldToChunkPos(worldPos);
        const int viewDistance = std::max<int>(2, runtime.gameplay.player->renderDistance);
        const int64_t radius2 =
            static_cast<int64_t>(viewDistance) * static_cast<int64_t>(viewDistance);
        const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
        const int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

        const auto &chunks = runtime.gameplay.chunkManager->getChunks();
        size_t desired = 0;
        size_t loaded = 0;
        std::vector<glm::ivec3> missingSamples;
        missingSamples.reserve(8);
        for (int x = centerChunk.x - viewDistance; x <= centerChunk.x + viewDistance; ++x) {
            const int64_t dx = static_cast<int64_t>(x - centerChunk.x);
            const int64_t dx2 = dx * dx;
            for (int z = centerChunk.z - viewDistance; z <= centerChunk.z + viewDistance; ++z) {
                const int64_t dz = static_cast<int64_t>(z - centerChunk.z);
                if (dx2 + dz * dz > radius2) {
                    continue;
                }
                for (int y = minChunkY; y <= maxChunkY; ++y) {
                    const glm::ivec3 cp(x, y, z);
                    if (!runtime.gameplay.chunkManager->inBounds(cp)) {
                        continue;
                    }
                    ++desired;
                    if (chunks.find(cp) != chunks.end()) {
                        ++loaded;
                    } else if (missingSamples.size() < 8) {
                        missingSamples.push_back(cp);
                    }
                }
            }
        }

        std::cerr << "[chunk/client] coverage center=(" << centerChunk.x << "," << centerChunk.y
                  << "," << centerChunk.z << ")"
                  << " viewDist=" << viewDistance << " desired=" << desired << " loaded=" << loaded
                  << " missing=" << (desired - loaded) << " queue(data/delta/unload)=("
                  << queueDepths.chunkData << "/" << queueDepths.chunkDelta << "/"
                  << queueDepths.chunkUnload << ")"
                  << " applied(data/delta/unload)=(" << chunkDataApplied << "/" << chunkDeltaApplied
                  << "/" << chunkUnloadApplied << ")\n";

        if (!missingSamples.empty()) {
            std::cerr << "[chunk/client] missing samples:";
            for (const glm::ivec3 &cp : missingSamples) {
                std::cerr << " (" << cp.x << "," << cp.y << "," << cp.z << ")";
            }
            std::cerr << "\n";
        }
    }
}
