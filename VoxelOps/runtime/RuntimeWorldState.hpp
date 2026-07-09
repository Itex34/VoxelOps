#pragma once

#include <glm/ext/vector_int3.hpp>
#include <glm/vec3.hpp>

#include "../../Shared/player/BlockPlace.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct RuntimeWorldState {
    struct WorldItemVisual {
        uint64_t id = 0;
        uint16_t itemId = 0;
        uint16_t quantity = 0;
        glm::vec3 position{0.0f};
        glm::vec3 targetPosition{0.0f};
        glm::vec3 velocity{0.0f};
    };

    struct PendingBlockPlaceEdit {
        glm::ivec3 worldPos{0};
        uint8_t oldBlockId = 0;
        uint8_t newBlockId = 0;
    };

    struct PendingBlockPlaceRequest {
        std::vector<glm::ivec3> affectedChunks;
        std::vector<PendingBlockPlaceEdit> edits;
        double createdAt = 0.0;
    };

    struct PendingBlockBreakEdit {
        glm::ivec3 worldPos{0};
        uint8_t oldBlockId = 0;
    };

    struct PendingBlockBreakRequest {
        std::vector<glm::ivec3> affectedChunks;
        std::vector<PendingBlockBreakEdit> edits;
        double createdAt = 0.0;
    };

    static constexpr double ChunkRequestSendInterval = 0.5;
    static constexpr double ChunkRequestCenterChangeMinInterval = 1.0 / 8.0;
    static constexpr size_t MaxChunkDataApplyPerFrame = 8;
    static constexpr size_t MaxChunkDeltaApplyPerFrame = 48;
    static constexpr size_t MaxChunkUnloadApplyPerFrame = 64;
    static constexpr int64_t ChunkApplyBudgetUs = 4500;
    static constexpr int64_t ChunkApplyBudgetUsUnderInputPressure = 1500;
    static constexpr size_t MaxChunkMeshBuildsPerFrame = 4;
    static constexpr size_t MaxChunkMeshBuildsPerFrameUnderInputPressure = 2;
    static constexpr int64_t ChunkMeshBuildBudgetUs = 3000;
    static constexpr int64_t ChunkMeshBuildBudgetUsUnderInputPressure = 1200;
    static constexpr size_t MaxBlockPlaceResultsPerFrame = 32;
    static constexpr size_t MaxBlockBreakResultsPerFrame = 32;
    static constexpr double RespawnMissingChunkGraceSeconds = 1.25;
    static constexpr double RespawnDiagDurationSeconds = 10.0;

    std::unordered_map<uint64_t, WorldItemVisual> worldItems;
    uint32_t lastWorldItemSnapshotTick = 0;

    std::unordered_map<uint32_t, PendingBlockPlaceRequest> pendingBlockPlaceRequests;
    uint32_t nextBlockPlaceRequestId = 1;
    BlockPlace::BlockMode blockPlaceMode = BlockPlace::BlockMode::Block;
    std::unordered_map<uint32_t, PendingBlockBreakRequest> pendingBlockBreakRequests;
    uint32_t nextBlockBreakRequestId = 1;

    double lastChunkRequestSendTime = 0.0;
    double lastChunkCoverageLogTime = 0.0;
    glm::ivec3 lastChunkRequestCenter{0};
    bool hasLastChunkRequestCenter = false;
    bool renderStateNeedsResync = false;
    bool justRespawned = false;
    double respawnMissingChunkGraceUntil = 0.0;
    bool rbDiagActive = false;
    double rbDiagUntil = 0.0;
    double rbDiagNextHeartbeatAt = 0.0;
};
