#include "ChunkStreamingClient.hpp"

#include "../application/AppHelpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AppHelpers;

namespace {

    constexpr bool kTraceChunkLifecycle = false;
    constexpr bool kTracePersistentMissingChunks = true;
    constexpr double kPersistentMissingWarnSec = 3.0;
    constexpr double kPersistentMissingRelogSec = 2.0;
    constexpr size_t kMaxPersistentMissingLogsPerAudit = 8;

    struct MissingChunkTrace {
        double firstSeenSec = 0.0;
        double lastSeenSec = 0.0;
        double lastLogSec = 0.0;
        bool logged = false;
    };

    std::unordered_map<glm::ivec3, MissingChunkTrace, IVec3Hash, IVec3Eq> &
    MissingChunkTraceMap() {
        static std::unordered_map<glm::ivec3, MissingChunkTrace, IVec3Hash, IVec3Eq> traces;
        return traces;
    }

    void NoteChunkResolved(const glm::ivec3 &chunkPos, const char *reason) {
        if (!kTracePersistentMissingChunks) {
            return;
        }
        auto &traces = MissingChunkTraceMap();
        auto it = traces.find(chunkPos);
        if (it == traces.end()) {
            return;
        }

        const double nowSec = GetTimeSeconds();
        if (it->second.logged) {
            std::cerr << "[chunk/missing] resolved chunk=(" << chunkPos.x << "," << chunkPos.y
                      << "," << chunkPos.z << ") reason=" << reason
                      << " missingForSec=" << (nowSec - it->second.firstSeenSec) << "\n";
        }
        traces.erase(it);
    }

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
    constexpr size_t kMaxCoverageResyncRequestsPerAudit = 16;
    constexpr size_t kCoverageResyncIdleAuditsRequired = 1;
    constexpr size_t kInitialChunkFillLoadedThreshold = 160;
    constexpr size_t kInitialChunkFillDataApplyCap = 32;
    constexpr size_t kInitialChunkFillMeshBuildCap = 8;
    constexpr int64_t kInitialChunkFillApplyBudgetUs = 7000;
    constexpr int64_t kInitialChunkFillMeshBudgetUs = 5000;

    const auto requestChunkResync =
        [&](const glm::ivec3 &chunkPos, bool force, bool useGlobalThrottle = true) -> bool {
        const double nowSec = GetTimeSeconds();
        if (!runtime.network.clientNet.IsConnected()) {
            return false;
        }
        if (useGlobalThrottle && nowSec < m_nextChunkResyncSendAt) {
            return false;
        }
        auto it = m_chunkResyncCooldownUntil.find(chunkPos);
        if (!force && it != m_chunkResyncCooldownUntil.end() && nowSec < it->second) {
            return false;
        }
        if (useGlobalThrottle) {
            m_nextChunkResyncSendAt = nowSec + kChunkResyncGlobalIntervalSec;
        }
        m_chunkResyncCooldownUntil[chunkPos] = nowSec + kChunkResyncCooldownSec;
        if (!runtime.network.clientNet.SendChunkResyncRequest(chunkPos)) {
            static double s_nextChunkResyncFailureLogAt = 0.0;
            if (kEnableChunkDiagnostics && nowSec >= s_nextChunkResyncFailureLogAt) {
                s_nextChunkResyncFailureLogAt = nowSec + 1.0;
                std::cerr << "[chunk/resync] failed to request full chunk (" << chunkPos.x << ","
                          << chunkPos.y << "," << chunkPos.z << ") status="
                          << runtime.network.clientNet.GetConnectionStatusText() << "\n";
            }
            return false;
        }
        return true;
    };

    const ClientNetwork::ChunkQueueDepths initialQueueDepths =
        runtime.network.clientNet.GetChunkQueueDepths();
    const bool initialChunkFill =
        runtime.gameplay.chunkManager->getChunks().size() < kInitialChunkFillLoadedThreshold;
    const bool chunkDataBacklog = initialQueueDepths.chunkData > 256;
    int64_t chunkApplyBudgetUs =
        chunkDataBacklog
            ? (prioritizeMovement ? RuntimeWorldState::ChunkApplyBudgetUsUnderInputPressure * 2
                                  : RuntimeWorldState::ChunkApplyBudgetUs * 2)
            : (prioritizeMovement ? RuntimeWorldState::ChunkApplyBudgetUsUnderInputPressure
                                  : RuntimeWorldState::ChunkApplyBudgetUs);
    size_t maxChunkDataApplyThisFrame =
        chunkDataBacklog ? (prioritizeMovement ? RuntimeWorldState::MaxChunkDataApplyPerFrame
                                               : RuntimeWorldState::MaxChunkDataApplyPerFrame * 3)
                         : RuntimeWorldState::MaxChunkDataApplyPerFrame;
    if (initialChunkFill) {
        chunkApplyBudgetUs = std::max(chunkApplyBudgetUs, kInitialChunkFillApplyBudgetUs);
        maxChunkDataApplyThisFrame =
            std::max(maxChunkDataApplyThisFrame, kInitialChunkFillDataApplyCap);
    }
    const size_t maxChunkStreamEventsThisFrame =
        maxChunkDataApplyThisFrame + RuntimeWorldState::MaxChunkDeltaApplyPerFrame +
        RuntimeWorldState::MaxChunkUnloadApplyPerFrame;
    const auto chunkApplyStart = std::chrono::steady_clock::now();
    const auto withinChunkApplyBudget = [&]() -> bool {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - chunkApplyStart
        )
                                   .count();
        return elapsedUs < chunkApplyBudgetUs;
    };
    size_t chunkDataApplied = 0;
    size_t chunkDeltaApplied = 0;
    size_t chunkUnloadApplied = 0;
    size_t chunkStreamEventsApplied = 0;
    std::vector<glm::ivec3> appliedChunkReceipts;
    appliedChunkReceipts.reserve(maxChunkDataApplyThisFrame);
    while (chunkStreamEventsApplied < maxChunkStreamEventsThisFrame && withinChunkApplyBudget()) {
        ClientNetwork::ChunkStreamEvent event;
        if (!runtime.network.clientNet.PopChunkStreamEvent(event)) {
            break;
        }

        switch (event.type) {
        case ClientNetwork::ChunkStreamEventType::Data:
            if (kTraceChunkLifecycle) {
                std::cerr << "[chunk/client] pop data chunk=(" << event.chunkData.chunkX << ","
                          << event.chunkData.chunkY << "," << event.chunkData.chunkZ
                          << ") version=" << event.chunkData.version << "\n";
            }
            if (runtime.gameplay.chunkManager->applyNetworkChunkData(event.chunkData)) {
                NoteChunkResolved(
                    glm::ivec3(
                        event.chunkData.chunkX, event.chunkData.chunkY, event.chunkData.chunkZ
                    ),
                    "data-applied"
                );
                appliedChunkReceipts.emplace_back(
                    event.chunkData.chunkX, event.chunkData.chunkY, event.chunkData.chunkZ
                );
            } else if (kTraceChunkLifecycle) {
                std::cerr << "[chunk/client] apply data failed chunk=(" << event.chunkData.chunkX
                          << "," << event.chunkData.chunkY << "," << event.chunkData.chunkZ
                          << ") version=" << event.chunkData.version << "\n";
            }
            ++chunkDataApplied;
            break;
        case ClientNetwork::ChunkStreamEventType::Delta: {
            const NetworkChunkDeltaApplyResult deltaResult =
                runtime.gameplay.chunkManager->applyNetworkChunkDelta(event.chunkDelta);
            if (deltaResult == NetworkChunkDeltaApplyResult::MissingBaseChunk) {
                requestChunkResync(
                    glm::ivec3(
                        event.chunkDelta.chunkX, event.chunkDelta.chunkY, event.chunkDelta.chunkZ
                    ),
                    true,
                    false
                );
            }
            ++chunkDeltaApplied;
            break;
        }
        case ClientNetwork::ChunkStreamEventType::Unload:
            if (kTraceChunkLifecycle) {
                std::cerr << "[chunk/client] pop unload chunk=(" << event.chunkUnload.chunkX << ","
                          << event.chunkUnload.chunkY << "," << event.chunkUnload.chunkZ << ")\n";
            }
            runtime.gameplay.chunkManager->applyNetworkChunkUnload(event.chunkUnload);
            ++chunkUnloadApplied;
            break;
        }
        ++chunkStreamEventsApplied;
    }
    if (!appliedChunkReceipts.empty()) {
        if (kTraceChunkLifecycle) {
            std::cerr << "[chunk/client] ack batch count=" << appliedChunkReceipts.size()
                      << " first=(" << appliedChunkReceipts.front().x << ","
                      << appliedChunkReceipts.front().y << "," << appliedChunkReceipts.front().z
                      << ")\n";
        }
        (void)runtime.network.clientNet.SendChunkHaveBatch(appliedChunkReceipts);
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
                requestChunkResync(chunkPos, true, false);
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
                requestChunkResync(chunkPos, true, false);
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
                requestChunkResync(chunkPos, true, false);
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
                requestChunkResync(chunkPos, true, false);
            }
            it = runtime.world.pendingBlockBreakRequests.erase(it);
        } else {
            ++it;
        }
    }

    if (runtime.network.clientNet.IsConnected() && runtime.gameplay.player != nullptr &&
        runtime.gameplay.player->isGrounded() &&
        std::abs(runtime.gameplay.player->getVelocity().y) < 0.01f) {
        const glm::vec3 playerPos = runtime.gameplay.player->getPosition();
        const int footX = static_cast<int>(std::floor(playerPos.x));
        const int footZ = static_cast<int>(std::floor(playerPos.z));
        const int belowY = static_cast<int>(std::floor(playerPos.y - 0.05f));
        const glm::ivec3 belowWorld(footX, belowY, footZ);
        const glm::ivec3 belowChunk =
            runtime.gameplay.chunkManager->worldToChunkPos(belowWorld);

        if (runtime.gameplay.chunkManager->inBounds(belowChunk) &&
            runtime.gameplay.chunkManager->hasChunkLoaded(belowChunk) &&
            runtime.gameplay.chunkManager->getBlockGlobal(footX, belowY, footZ) == BlockID::Air) {
            const std::optional<NetworkChunkDebugSummary> summary =
                runtime.gameplay.chunkManager->getNetworkChunkDebugSummary(belowChunk);
            if (summary.has_value() && summary->nonAirCount > 0 &&
                requestChunkResync(belowChunk, false, false)) {
                std::cerr << "[chunk/resync] loaded-air-underfoot request chunk=("
                          << belowChunk.x << "," << belowChunk.y << "," << belowChunk.z << ")"
                          << " pos=(" << playerPos.x << "," << playerPos.y << "," << playerPos.z
                          << ") belowBlock=(" << footX << "," << belowY << "," << footZ << ")"
                          << " chunkVersion=" << summary->version
                          << " chunkHash=" << summary->rawContentHash
                          << " chunkNonAir=" << summary->nonAirCount
                          << " chunkNonAirY=(" << summary->minNonAirWorldY << ","
                          << summary->maxNonAirWorldY << ")\n";
            }
        }
    }

    size_t maxChunkMeshBuilds =
        prioritizeMovement ? RuntimeWorldState::MaxChunkMeshBuildsPerFrameUnderInputPressure
                           : RuntimeWorldState::MaxChunkMeshBuildsPerFrame;
    int64_t chunkMeshBuildBudgetUs =
        prioritizeMovement ? RuntimeWorldState::ChunkMeshBuildBudgetUsUnderInputPressure
                           : RuntimeWorldState::ChunkMeshBuildBudgetUs;
    if (initialChunkFill) {
        maxChunkMeshBuilds = std::max(maxChunkMeshBuilds, kInitialChunkFillMeshBuildCap);
        chunkMeshBuildBudgetUs = std::max(chunkMeshBuildBudgetUs, kInitialChunkFillMeshBudgetUs);
    }
    runtime.gameplay.chunkManager->updateDirtyChunks(maxChunkMeshBuilds, chunkMeshBuildBudgetUs);

    const double now = GetTimeSeconds();
    if (runtime.network.clientNet.IsConnected() &&
        now - runtime.world.lastChunkCoverageLogTime >= 1.0) {
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
        std::vector<glm::ivec3> missingChunks;
        missingChunks.reserve(64);
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
                    } else {
                        missingChunks.push_back(cp);
                    }
                }
            }
        }

        const bool streamIdle = queueDepths.chunkData == 0 && chunkDataApplied == 0;
        if (!missingChunks.empty() && streamIdle) {
            ++runtime.world.chunkCoverageMissingIdleAudits;
        } else {
            runtime.world.chunkCoverageMissingIdleAudits = 0;
            if (missingChunks.empty()) {
                runtime.world.chunkCoverageMissingScanOffset = 0;
            }
        }

        size_t coverageResyncRequestsSent = 0;
        if (runtime.network.clientNet.IsConnected() &&
            runtime.world.chunkCoverageMissingIdleAudits >= kCoverageResyncIdleAuditsRequired &&
            !missingChunks.empty()) {
            std::sort(
                missingChunks.begin(),
                missingChunks.end(),
                [&](const glm::ivec3 &a, const glm::ivec3 &b) {
                    const int64_t adx = static_cast<int64_t>(a.x - centerChunk.x);
                    const int64_t adz = static_cast<int64_t>(a.z - centerChunk.z);
                    const int64_t bdx = static_cast<int64_t>(b.x - centerChunk.x);
                    const int64_t bdz = static_cast<int64_t>(b.z - centerChunk.z);
                    const int64_t aHoriz = adx * adx + adz * adz;
                    const int64_t bHoriz = bdx * bdx + bdz * bdz;
                    if (aHoriz != bHoriz) {
                        return aHoriz < bHoriz;
                    }

                    const int aVertical = std::abs(a.y - centerChunk.y);
                    const int bVertical = std::abs(b.y - centerChunk.y);
                    if (aVertical != bVertical) {
                        return aVertical < bVertical;
                    }
                    if (a.y != b.y) {
                        return a.y < b.y;
                    }
                    if (a.x != b.x) {
                        return a.x < b.x;
                    }
                    return a.z < b.z;
                }
            );
            const size_t startOffset =
                runtime.world.chunkCoverageMissingScanOffset % missingChunks.size();
            for (size_t i = 0;
                 i < missingChunks.size() &&
                 coverageResyncRequestsSent < kMaxCoverageResyncRequestsPerAudit;
                 ++i) {
                const glm::ivec3 &cp = missingChunks[(startOffset + i) % missingChunks.size()];
                if (requestChunkResync(cp, false, false)) {
                    ++coverageResyncRequestsSent;
                }
            }
            runtime.world.chunkCoverageMissingScanOffset =
                (startOffset + kMaxCoverageResyncRequestsPerAudit) % missingChunks.size();
        }

        if (kEnableChunkDiagnostics) {
            std::cerr << "[chunk/client] coverage center=(" << centerChunk.x << ","
                      << centerChunk.y << "," << centerChunk.z << ")"
                      << " viewDist=" << viewDistance << " desired=" << desired
                      << " loaded=" << loaded << " missing=" << (desired - loaded)
                      << " queue(data/delta/unload)=(" << queueDepths.chunkData << "/"
                      << queueDepths.chunkDelta << "/" << queueDepths.chunkUnload << ")"
                      << " applied(data/delta/unload)=(" << chunkDataApplied << "/"
                      << chunkDeltaApplied << "/" << chunkUnloadApplied << ")"
                      << " coverageIdleAudits="
                      << runtime.world.chunkCoverageMissingIdleAudits
                      << " coverageResyncSent=" << coverageResyncRequestsSent << "\n";
        }

        if (kEnableChunkDiagnostics && !missingChunks.empty()) {
            std::cerr << "[chunk/client] missing samples:";
            const size_t sampleCount = std::min<size_t>(missingChunks.size(), 8);
            for (size_t i = 0; i < sampleCount; ++i) {
                const glm::ivec3 &cp = missingChunks[i];
                std::cerr << " (" << cp.x << "," << cp.y << "," << cp.z << ")";
            }
            std::cerr << "\n";
        }

        if (kTracePersistentMissingChunks && !missingChunks.empty()) {
            auto &traces = MissingChunkTraceMap();
            const double traceNowSec = GetTimeSeconds();
            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> currentMissing;
            currentMissing.reserve(missingChunks.size());
            size_t persistentLogsThisAudit = 0;
            for (const glm::ivec3 &cp : missingChunks) {
                currentMissing.insert(cp);
                auto [it, inserted] = traces.try_emplace(cp);
                MissingChunkTrace &trace = it->second;
                if (inserted) {
                    trace.firstSeenSec = traceNowSec;
                    trace.lastLogSec = traceNowSec;
                }
                trace.lastSeenSec = traceNowSec;

                const double missingForSec = traceNowSec - trace.firstSeenSec;
                const bool warnNow =
                    missingForSec >= kPersistentMissingWarnSec &&
                    (!trace.logged ||
                     traceNowSec - trace.lastLogSec >= kPersistentMissingRelogSec);
                if (warnNow) {
                    trace.logged = true;
                    trace.lastLogSec = traceNowSec;
                    if (persistentLogsThisAudit < kMaxPersistentMissingLogsPerAudit) {
                        ++persistentLogsThisAudit;
                        std::cerr << "[chunk/missing] persistent chunk=(" << cp.x << "," << cp.y
                                  << "," << cp.z << ")"
                                  << " missingForSec=" << missingForSec
                                  << " queue(data/delta/unload)=(" << queueDepths.chunkData << "/"
                                  << queueDepths.chunkDelta << "/" << queueDepths.chunkUnload
                                  << ")"
                                  << " applied(data/delta/unload)=(" << chunkDataApplied << "/"
                                  << chunkDeltaApplied << "/" << chunkUnloadApplied << ")"
                                  << " desiredMissing=" << missingChunks.size()
                                  << " suppressedThisAudit="
                                  << (missingChunks.size() > persistentLogsThisAudit
                                          ? (missingChunks.size() - persistentLogsThisAudit)
                                          : 0)
                                  << "\n";
                    }
                }
            }

            for (auto it = traces.begin(); it != traces.end();) {
                if (currentMissing.find(it->first) == currentMissing.end()) {
                    it = traces.erase(it);
                } else {
                    ++it;
                }
            }
        } else if (kTracePersistentMissingChunks && missingChunks.empty()) {
            MissingChunkTraceMap().clear();
        }
    }
}
