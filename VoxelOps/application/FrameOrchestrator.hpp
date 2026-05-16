#pragma once

#include "../ui/Hud.hpp"
#include "ClientInputSystem.hpp"
#include "ClientSession.hpp"
#include "FrameServices.hpp"
#include "../runtime/ClientPrediction.hpp"
#include "../render/RenderSceneBuilder.hpp"
#include "../systems/CombatShootSystem.hpp"
#include "../world/ChunkStreamingClient.hpp"
#include "../systems/WorldInteractionSystem.hpp"
#include "../runtime/Runtime.hpp"

#include "../../Shared/gun/GunType.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include <glm/vec3.hpp>

struct ImDrawData;
class Camera;
struct SDL_Window;

struct HostFrameContext {
    SDL_Window *window = nullptr; // required, non-owning
};

struct SimulationFrameContext {
    bool *useDebugCamera = nullptr;        // required, non-owning
    bool *forceCursorEnabled = nullptr;    // required, non-owning
    bool *wasWorldInteractPressed = nullptr; // required, non-owning
};

struct UiFrameContext {
    std::string *serverIp = nullptr;       // required, non-owning
    uint16_t *serverPort = nullptr;        // required, non-owning
    std::string *requestedUsername = nullptr; // required, non-owning
    bool *showDebugUi = nullptr;           // required, non-owning
    bool *showInventoryUi = nullptr;       // required, non-owning
    bool *enableRawMouseInput = nullptr;   // required, non-owning
    bool *requestSwitchToOpenGl = nullptr; // required, non-owning
    bool *requestSwitchToVulkan = nullptr; // required, non-owning
    int *renderApiPreference = nullptr;    // required, non-owning, 0=OpenGL, 1=Vulkan
};

struct RenderFrameContext {
    bool *toggleWireframe = nullptr; // optional, non-owning
    bool *toggleChunkBorders = nullptr; // optional, non-owning
    bool *toggleDebugFrustum = nullptr; // optional, non-owning
    float *skyExposure = nullptr; // required, non-owning
    glm::vec3 *sunDirection = nullptr; // required, non-owning
    glm::vec3 *sunShadowDirectionalBias = nullptr; // required, non-owning
    float *sunShadowLowSunBiasBoost = nullptr; // required, non-owning
    bool *sunShadowFrontFaceCullAtLowSun = nullptr; // required, non-owning
    float *sunShadowFrontFaceCullGrazingThreshold = nullptr; // required, non-owning
};

struct FrameOrchestratorContext {
    FrameInputHost *inputHost = nullptr; // required, non-owning
    FrameConnectionHost *connectionHost = nullptr; // required, non-owning
    FrameWindowHost *windowHost = nullptr; // required, non-owning
    FrameRenderHost *renderHost = nullptr; // required, non-owning
    HostFrameContext host;
    SimulationFrameContext simulation;
    UiFrameContext ui;
    RenderFrameContext render;
};

class FrameOrchestrator {
public:
    FrameOrchestrator() = default;
    void bind(Runtime &runtime, FrameOrchestratorContext context);

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
    Runtime &runtime();
    const Runtime &runtime() const;

    Runtime *m_runtime = nullptr;
    FrameOrchestratorContext m_context;
    ClientInputSystem m_inputSystem;
    ClientPrediction m_clientPrediction;

    
    ClientSession m_clientSession;
    Hud m_hudSystem;
    WorldInteractionSystem m_worldInteractionSystem;
    CombatShootSystem m_combatShootSystem;
    ChunkStreamingClient m_chunkStreaming;
    RenderSceneBuilder m_renderSceneBuilder;
    double m_frameNow = 0.0;
};
