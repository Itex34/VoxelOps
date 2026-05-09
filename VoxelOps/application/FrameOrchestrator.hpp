#pragma once

#include "../ui/Hud.hpp"
#include "ClientInputSystem.hpp"
#include "ClientNetworkSystem.hpp"
#include "../runtime/ClientPrediction.hpp"
#include "../render/RenderSceneBuilder.hpp"
#include "../systems/CombatShootSystem.hpp"
#include "../world/ChunkStreamingClient.hpp"
#include "../systems/WorldInteractionSystem.hpp"
#include "../runtime/Runtime.hpp"

#include "../../Shared/gun/GunType.hpp"

#include <functional>
#include <cstddef>

struct ImDrawData;
class Camera;
struct SDL_Window;

struct HostFrameContext {
    SDL_Window *window = nullptr;
    std::function<void(Runtime &)> updateDebugCamera;
    std::function<void(Runtime &)> updateToggleStates;
    std::function<void(Runtime &)> pollEvents;
    std::function<void()> applyMouseInputModes;
    std::function<void()> updateFPSCounter;
    std::function<void(SDL_Window *)> toggleFullscreen;
};

struct SimulationFrameContext {
    bool *useDebugCamera = nullptr;
    bool *forceCursorEnabled = nullptr;
    bool *wasWorldInteractPressed = nullptr;

    std::function<bool(Runtime &)> beginConnectionAttempt;
    std::function<bool(Runtime &, GunType)> equipGun;
};

struct UiFrameContext {
    std::string *serverIp = nullptr;
    uint16_t *serverPort = nullptr;
    std::string *requestedUsername = nullptr;
    bool *showDebugUi = nullptr;
    bool *showInventoryUi = nullptr;
    bool *enableRawMouseInput = nullptr;
};

struct RenderFrameContext {
    bool *toggleWireframe = nullptr;
    bool *toggleChunkBorders = nullptr;
    bool *toggleDebugFrustum = nullptr;
    float *skyExposure = nullptr;
    glm::vec3 *sunDirection = nullptr;
    glm::vec3 *sunShadowDirectionalBias = nullptr;
    float *sunShadowLowSunBiasBoost = nullptr;
    bool *sunShadowFrontFaceCullAtLowSun = nullptr;
    float *sunShadowFrontFaceCullGrazingThreshold = nullptr;

    std::function<void(Runtime &, const Camera &)> renderWorldItems;
};

struct FrameOrchestratorContext {
    HostFrameContext host;
    SimulationFrameContext simulation;
    UiFrameContext ui;
    RenderFrameContext render;
};

class FrameOrchestrator {
public:
    FrameOrchestrator(Runtime &runtime, FrameOrchestratorContext context);

    void runFrame();

private:
    struct SimulationStageResult {
        size_t localPredictionSteps = 0;
        RenderDeviceCapabilities renderCapabilities{};
    };

    struct UiStageResult {
        ImDrawData *drawData = nullptr;
    };

    SimulationStageResult runSimulationStage();
    UiStageResult runUiStage(const SimulationStageResult &simulation);
    void runRenderStage(const SimulationStageResult &simulation, const UiStageResult &ui);
    void runPresentStage(size_t localPredictionSteps);
    void updateFrameTime();
    void updateFrameHotkeysAndCounters();

    Runtime &m_runtime;
    FrameOrchestratorContext m_context;
    ClientInputSystem m_inputSystem;
    ClientPrediction m_clientPrediction;

    ClientNetworkSystem m_networkSystem;
    Hud m_hudSystem;
    WorldInteractionSystem m_worldInteractionSystem;
    CombatShootSystem m_combatShootSystem;
    ChunkStreamingClient m_chunkStreaming;
    RenderSceneBuilder m_renderSceneBuilder;
    double m_frameNow = 0.0;
};
