#pragma once

#include "../data/GameData.hpp"
#include "../graphics/Camera.hpp"
#include "../world/ChunkManager.hpp"
#include "../graphics/Frustum.hpp"
#include "../graphics/IGunRenderer.hpp"
#include "../graphics/IGunSceneRenderer.hpp"
#include "../graphics/IRenderDevice.hpp"
#include "../graphics/ISkyBackend.hpp"
#include "../graphics/OpenGL/Shader.hpp"
#include "../input/InputCallbacks.hpp"
#include "../network/ClientNetwork.hpp"
#include "../physics/RayManager.hpp"
#include "../physics/Raycast.hpp"
#include "../player/Player.hpp"
#include "../runtime/ClientReconciler.hpp"
#include "../runtime/RuntimeCombatState.hpp"
#include "../runtime/RuntimeConnectionState.hpp"
#include "../runtime/RuntimeInputState.hpp"
#include "../runtime/RuntimePerfState.hpp"
#include "../runtime/SnapshotInterpolator.hpp"
#include "../ui/debug/DebugUi.hpp"
#include "../ui/player/InventoryUI.hpp"
#include "../../Shared/player/PlayerData.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct CallbackContext {
    InputCallbacks *inputCallbacks = nullptr;
    ISkyBackend *skyBackend = nullptr;
    bool *useDebugCamera = nullptr;
};

struct Runtime {
    std::unique_ptr<IRenderDevice> renderer;
    std::unique_ptr<ChunkManager> chunkManager;
    std::unique_ptr<Player> player;
    std::unique_ptr<InputCallbacks> inputCallbacks;

    RayManager rayManager;
    ClientNetwork clientNet;
    SnapshotInterpolator snapshotInterpolator;
    ClientReconciler reconciler;

    std::unique_ptr<Shader> chunkShader;
    std::unique_ptr<Shader> dbgShader;
    std::unique_ptr<ISkyBackend> sky;
    std::shared_ptr<IGunRenderer> gunRenderer;
    std::unique_ptr<IGunSceneRenderer> gunSceneRenderer;
    std::unique_ptr<DebugUi> debugUi;
    std::unique_ptr<InventoryUI> inventoryUi;

    Frustum frustum;
    Camera debugCamera{glm::vec3(0.0f, 100.0f, 0.0f)};
    Camera interpolatedPlayerCamera{glm::vec3(0.0f)};

    bool supportsGL43Shaders = false;
    bool chunkUniformsInitialized = false;

    uint32_t inputTickCounter = 1;
    uint64_t localPlayerId = 0;
    bool hasLocalPlayerId = false;
    uint32_t lastAckedInputTick = 0;
    uint32_t lastAppliedServerTick = 0;
    bool hasAppliedServerTick = false;
    uint32_t lastReceivedSelfSnapshotTick = 0;
    bool hasReceivedSelfSnapshotTick = false;
    double localSimAccumulator = 0.0;
    struct PendingInputEntry {
        PlayerInput packet{};
        double deltaSeconds = 0.0;
    };
    struct KillFeedEntry {
        std::string killer;
        std::string victim;
        uint16_t weaponId = 0;
        double expiresAt = 0.0;
    };
    struct WorldItemVisual {
        uint64_t id = 0;
        uint16_t itemId = 0;
        uint16_t quantity = 0;
        glm::vec3 position{0.0f};
        glm::vec3 targetPosition{0.0f};
        glm::vec3 velocity{0.0f};
    };
    std::deque<PendingInputEntry> pendingInputs;
    static constexpr size_t MaxPendingInputs = 256;
    std::deque<KillFeedEntry> killFeedEntries;
    static constexpr size_t MaxKillFeedEntries = 8;
    static constexpr double KillFeedDurationSec = 5.0;
    std::unordered_map<uint64_t, WorldItemVisual> worldItems;
    uint32_t lastWorldItemSnapshotTick = 0;
    int matchRemainingSeconds = 600;
    bool matchStarted = false;
    bool matchEnded = false;
    std::string matchWinner;
    std::vector<ClientNetwork::ScoreboardEntry> scoreboardEntries;
    double lastInputSendTime = 0.0;
    double lastChunkRequestSendTime = 0.0;
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
    std::unordered_map<uint32_t, PendingBlockPlaceRequest> pendingBlockPlaceRequests;
    uint32_t nextBlockPlaceRequestId = 1;
    std::unordered_map<uint32_t, PendingBlockBreakRequest> pendingBlockBreakRequests;
    uint32_t nextBlockBreakRequestId = 1;
    static constexpr double InputSendInterval = 1.0 / 60.0; // 60 Hz
    static constexpr double LocalPredictionStep =
        1.0 / 60.0; // match authoritative server tick for replay parity
    static constexpr size_t MaxLocalPredictionStepsPerFrame = 8;
    static constexpr float BasicAuthReconcileDeadzone = 0.08f; // For reconciliation threshold
    static constexpr float BasicAuthReconcileTeleportDistance =
        2.0f; // For large correction detection
    static constexpr float RenderLeadMaxDistance = 0.40f;
    static constexpr float RenderExtrapolationBlend = 0.60f;
    static constexpr float RenderExtrapolationSpeedMin = 0.20f;
    static constexpr float RenderExtrapolationSpeedMax = 1.10f;
    static constexpr float RenderCameraSmoothingGroundHz = 26.0f;
    static constexpr float RenderCameraSmoothingAirHz = 16.0f;
    static constexpr float RenderIdleSettleSpeedThreshold = 0.20f;
    static constexpr float RenderIdleSettleHz = 28.0f;
    static constexpr size_t InputRedundancyCopies = 1;
    static constexpr double ChunkRequestSendInterval =
        0.5; // 2 Hz baseline + immediate on center changes
    static constexpr double ChunkRequestCenterChangeMinInterval =
        1.0 / 8.0; // cap center-change churn during correction jitter
    static constexpr size_t MaxChunkDataApplyPerFrame = 12;
    static constexpr size_t MaxChunkDeltaApplyPerFrame = 48;
    static constexpr size_t MaxChunkUnloadApplyPerFrame = 64;
    static constexpr int64_t ChunkApplyBudgetUs = 9000;
    static constexpr int64_t ChunkApplyBudgetUsUnderInputPressure = 2500;
    static constexpr size_t MaxChunkMeshBuildsPerFrame = 8;
    static constexpr size_t MaxChunkMeshBuildsPerFrameUnderInputPressure = 3;
    static constexpr int64_t ChunkMeshBuildBudgetUs = 6000;
    static constexpr int64_t ChunkMeshBuildBudgetUsUnderInputPressure = 2000;
    static constexpr size_t MaxBlockPlaceResultsPerFrame = 32;
    static constexpr size_t MaxBlockBreakResultsPerFrame = 32;
    static constexpr double RespawnMissingChunkGraceSeconds = 1.25;
    static constexpr double RespawnDiagDurationSeconds = 10.0;
    double lastChunkCoverageLogTime = 0.0;
    glm::ivec3 lastChunkRequestCenter{0};
    bool hasLastChunkRequestCenter = false;
    bool renderStateNeedsResync = false;
    bool justRespawned = false;
    double respawnMissingChunkGraceUntil = 0.0;
    bool rbDiagActive = false;
    double rbDiagUntil = 0.0;
    double rbDiagNextHeartbeatAt = 0.0;
    Player::SimulationState renderPrevSimState{};
    Player::SimulationState renderCurrSimState{};
    bool hasRenderSimState = false;
    glm::vec3 smoothedPlayerCameraPos{0.0f};
    bool hasSmoothedPlayerCameraPos = false;
    RuntimePerfState perf;
    RuntimeInputState inputLook;
    RuntimeConnectionState connection;
    RuntimeCombatState combat;

    CallbackContext callbackContext;
};
