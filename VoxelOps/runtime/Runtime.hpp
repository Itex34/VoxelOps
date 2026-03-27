#pragma once

#include "../data/GameData.hpp"
#include "../graphics/Camera.hpp"
#include "../graphics/ChunkManager.hpp"
#include "../graphics/Frustum.hpp"
#include "../graphics/Renderer.hpp"
#include "../graphics/Shader.hpp"
#include "../graphics/Sky.hpp"
#include "../gun/Gun.hpp"
#include "../input/InputCallbacks.hpp"
#include "../network/ClientNetwork.hpp"
#include "../physics/RayManager.hpp"
#include "../physics/Raycast.hpp"
#include "../player/Player.hpp"
#include "../runtime/ClientReconciler.hpp"
#include "../runtime/SnapshotInterpolator.hpp"
#include "../ui/debug/DebugUi.hpp"
#include "../ui/player/InventoryUI.hpp"
#include "../../Shared/gun/GunType.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../../Shared/runtime/Paths.hpp"



struct CallbackContext {
    InputCallbacks* inputCallbacks = nullptr;
    bool* useDebugCamera = nullptr;
};

struct Runtime {
    Renderer renderer;
    std::unique_ptr<ChunkManager> chunkManager;
    std::unique_ptr<Player> player;
    std::unique_ptr<InputCallbacks> inputCallbacks;

    RayManager rayManager;
    ClientNetwork clientNet;
    SnapshotInterpolator snapshotInterpolator;
    ClientReconciler reconciler;

    std::unique_ptr<Shader> chunkShader;
    std::unique_ptr<Shader> dbgShader;
    std::unique_ptr<Shader> gunShader;
    Sky sky;
    std::unique_ptr<DebugUi> debugUi;
    std::unique_ptr<InventoryUI> inventoryUi;

    Frustum frustum;
    Camera debugCamera{ glm::vec3(0.0f, 100.0f, 0.0f) };
    Camera interpolatedPlayerCamera{ glm::vec3(0.0f) };

    bool supportsGL43Shaders = false;
    bool chunkUniformsInitialized = false;

    uint32_t inputTickCounter = 1;
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
        glm::vec3 position{ 0.0f };
        glm::vec3 targetPosition{ 0.0f };
        glm::vec3 velocity{ 0.0f };
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
    bool localPlayerAlive = true;
    float localHealth = 100.0f;
    float localRespawnSeconds = 0.0f;
    std::string localDeathKiller;
    bool wasRespawnClickDown = false;
    double lastInputSendTime = 0.0;
    double lastChunkRequestSendTime = 0.0;
    double lastShootSendTime = 0.0;
    double nextReconnectAttemptTime = 0.0;
    double reconnectBackoffSeconds = 1.0;
    std::string lastConnectionStatus = "disconnected";
    std::array<char, 128> pendingServerEndpointInput{};
    std::array<char, kMaxConnectUsernameChars + 1> pendingUsernameInput{};
    bool wasEndpointPasteShortcutPressed = false;
    std::string usernamePromptError;
    uint32_t nextClientShotId = 1;
    double shootSendInterval = 1.0 / 8.0;
    uint16_t activeHotbarSlot = 0;
    GunType equippedGunType = kDefaultGunType;
    std::unordered_map<uint16_t, std::unique_ptr<Gun>> preloadedGuns;
    Gun* equippedGun = nullptr;
    glm::vec3 equippedGunViewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
    glm::vec3 equippedGunViewScale = glm::vec3(0.10f);
    glm::vec3 equippedGunViewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
    static constexpr double InputSendInterval = 1.0 / 60.0; // 60 Hz
    static constexpr double LocalPredictionStep = 1.0 / 60.0; // match authoritative server tick for replay parity
    static constexpr size_t MaxLocalPredictionStepsPerFrame = 8;
    static constexpr float BasicAuthReconcileDeadzone = 0.08f;  // For reconciliation threshold
    static constexpr float BasicAuthReconcileTeleportDistance = 2.0f;  // For large correction detection
    static constexpr float RenderLeadMaxDistance = 0.40f;
    static constexpr float RenderExtrapolationBlend = 0.60f;
    static constexpr float RenderExtrapolationSpeedMin = 0.20f;
    static constexpr float RenderExtrapolationSpeedMax = 1.10f;
    static constexpr float RenderCameraSmoothingGroundHz = 26.0f;
    static constexpr float RenderCameraSmoothingAirHz = 16.0f;
    static constexpr float RenderIdleSettleSpeedThreshold = 0.20f;
    static constexpr float RenderIdleSettleHz = 28.0f;
    static constexpr size_t InputRedundancyCopies = 1;
    static constexpr double ChunkRequestSendInterval = 0.5; // 2 Hz baseline + immediate on center changes
    static constexpr double ChunkRequestCenterChangeMinInterval = 1.0 / 30.0; // up to 30 Hz on border crossings
    static constexpr size_t MaxChunkDataApplyPerFrame = 12;
    static constexpr size_t MaxChunkDeltaApplyPerFrame = 48;
    static constexpr size_t MaxChunkUnloadApplyPerFrame = 64;
    static constexpr int64_t ChunkApplyBudgetUs = 9000;
    static constexpr int64_t ChunkApplyBudgetUsUnderInputPressure = 2500;
    static constexpr size_t MaxChunkMeshBuildsPerFrame = 8;
    static constexpr size_t MaxChunkMeshBuildsPerFrameUnderInputPressure = 3;
    static constexpr int64_t ChunkMeshBuildBudgetUs = 6000;
    static constexpr int64_t ChunkMeshBuildBudgetUsUnderInputPressure = 2000;
    double lastChunkCoverageLogTime = 0.0;
    glm::ivec3 lastChunkRequestCenter{ 0 };
    bool hasLastChunkRequestCenter = false;
    bool renderStateNeedsResync = false;
    Player::SimulationState renderPrevSimState{};
    Player::SimulationState renderCurrSimState{};
    bool hasRenderSimState = false;
    glm::vec3 smoothedPlayerCameraPos{ 0.0f };
    bool hasSmoothedPlayerCameraPos = false;
    float perfFrameCpuMs = 0.0f;
    float perfInputMs = 0.0f;
    float perfNetworkMs = 0.0f;
    float perfPredictionMs = 0.0f;
    float perfGameplayMs = 0.0f;
    float perfRenderCpuMs = 0.0f;
    float perfPresentMs = 0.0f;
    float perfChunkStreamingMs = 0.0f;

    double lastX = 0.0;
    double lastY = 0.0;
    double xpos = 0.0;
    double ypos = 0.0;
    float yaw = 0.0f;
    float pitch = 0.0f;

    CallbackContext callbackContext;
};
