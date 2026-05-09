#include "FrameOrchestrator.hpp"

#include "AppHelpers.hpp"
#include "../graphics/IGunSceneRenderer.hpp"
#include "../render/RenderScene.hpp"
#include "../data/GameData.hpp"
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>

namespace {
    using Clock = std::chrono::steady_clock;

    float MeasureMs(const Clock::time_point &start, const Clock::time_point &end) {
        return static_cast<float>(
                   std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
               ) *
               0.001f;
    }

    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }
} // namespace

FrameOrchestrator::FrameOrchestrator(Runtime &runtime, FrameOrchestratorContext context)
    : m_runtime(runtime)
    , m_context(std::move(context)) {}

void FrameOrchestrator::runFrame() {
    const auto perfFrameStart = Clock::now();
    updateFrameTime();

    const SimulationStageResult simulation = runSimulationStage();

    const auto perfRenderStart = Clock::now();
    const UiStageResult ui = runUiStage(simulation);
    runRenderStage(simulation, ui);
    updateFrameHotkeysAndCounters();
    const auto perfRenderEnd = Clock::now();
    m_runtime.app.perf.renderCpuMs = MeasureMs(perfRenderStart, perfRenderEnd);

    runPresentStage(simulation.localPredictionSteps);

    const auto perfFrameEnd = Clock::now();
    m_runtime.app.perf.frameCpuMs = MeasureMs(perfFrameStart, perfFrameEnd);
}

void FrameOrchestrator::updateFrameTime() {
    m_frameNow = AppHelpers::GetTimeSeconds();
    double frameDeltaSeconds = m_frameNow - GameData::lastFrame;
    if (!std::isfinite(frameDeltaSeconds) || frameDeltaSeconds < 0.0) {
        frameDeltaSeconds = 0.0;
    }
    // Clamp hitches so interpolation and prediction do not overreact to one bad frame.
    if (frameDeltaSeconds > 0.1) {
        frameDeltaSeconds = 0.1;
    }

    static double s_lastFrameTimeLog = 0.0;
    constexpr double kFrameTimeLogThresholdMs = 12.5;
    constexpr double kFrameTimeLogCooldownSec = 0.5;
    const double frameMs = frameDeltaSeconds * 1000.0;
    if (frameMs >= kFrameTimeLogThresholdMs &&
        (m_frameNow - s_lastFrameTimeLog) >= kFrameTimeLogCooldownSec) {
        const ClientNetwork::ChunkQueueDepths queueDepths =
            m_runtime.network.clientNet.GetChunkQueueDepths();
        const double fps = frameMs > 0.0 ? (1000.0 / frameMs) : 0.0;
        std::cerr << "[frame] slow frameMs=" << frameMs << " fps=" << fps
                  << " queue(data/delta/unload)=(" << queueDepths.chunkData << "/"
                  << queueDepths.chunkDelta << "/" << queueDepths.chunkUnload << ")\n";
        s_lastFrameTimeLog = m_frameNow;
    }

    GameData::deltaTime = frameDeltaSeconds;
    GameData::lastFrame = m_frameNow;
}

FrameOrchestrator::SimulationStageResult FrameOrchestrator::runSimulationStage() {
    const auto perfInputStart = Clock::now();
    if (m_runtime.ui.debugUi) {
        m_runtime.ui.debugUi->beginFrame();
    }
    if (m_context.host.updateDebugCamera) {
        m_context.host.updateDebugCamera(m_runtime);
    }
    if (m_context.host.updateToggleStates) {
        m_context.host.updateToggleStates(m_runtime);
    }

    if (!m_runtime.network.clientNet.IsConnected()) {
        GameData::cursorEnabled = true;
    }
    GameData::gameplayInputEnabled =
        m_runtime.network.clientNet.IsConnected() && (m_context.ui.showDebugUi != nullptr) &&
        (m_context.simulation.forceCursorEnabled != nullptr) && !*m_context.ui.showDebugUi &&
        !*m_context.simulation.forceCursorEnabled;

    m_runtime.gameplay.inputCallbacks->processInput(m_context.host.window);
    if (m_context.host.applyMouseInputModes) {
        m_context.host.applyMouseInputModes();
    }
    const ClientInputIntent inputIntent = m_inputSystem.captureIntent(m_runtime, m_context.host.window);
    m_runtime.prediction.localSimAccumulator += GameData::deltaTime;
    const auto perfInputEnd = Clock::now();
    m_runtime.app.perf.inputMs = MeasureMs(perfInputStart, perfInputEnd);

    const double maxAccumulatedTime = RuntimePredictionState::LocalPredictionStep *
                                      static_cast<double>(RuntimePredictionState::MaxLocalPredictionStepsPerFrame);
    if (m_runtime.prediction.localSimAccumulator > maxAccumulatedTime) {
        m_runtime.prediction.localSimAccumulator = maxAccumulatedTime;
    }

    const auto perfNetworkStart = Clock::now();
    ClientNetworkSystemContext netCtx{};
    netCtx.forceCursorEnabled = m_context.simulation.forceCursorEnabled;
    netCtx.beginConnectionAttempt = m_context.simulation.beginConnectionAttempt;
    netCtx.equipGun = m_context.simulation.equipGun;
    m_networkSystem.update(m_runtime, netCtx, &inputIntent);
    const auto perfNetworkEnd = Clock::now();
    m_runtime.app.perf.networkMs = MeasureMs(perfNetworkStart, perfNetworkEnd);

    const auto perfPredictionStart = Clock::now();
    if (!m_runtime.prediction.hasRenderSimState) {
        const Player::SimulationState initialSimState = m_runtime.gameplay.player->captureSimulationState();
        const Player::PresentationState initialPresentationState =
            m_runtime.gameplay.player->capturePresentationState();
        m_runtime.prediction.renderPrevSimState = initialSimState;
        m_runtime.prediction.renderCurrSimState = initialSimState;
        m_runtime.prediction.renderPrevPresentationState = initialPresentationState;
        m_runtime.prediction.renderCurrPresentationState = initialPresentationState;
        m_runtime.prediction.hasRenderSimState = true;
    }

    size_t localPredictionSteps = 0;
    if (m_runtime.combat.localPlayerAlive) {
        while (m_runtime.prediction.localSimAccumulator >= RuntimePredictionState::LocalPredictionStep &&
               localPredictionSteps < RuntimePredictionState::MaxLocalPredictionStepsPerFrame) {
            m_clientPrediction.update(
                m_runtime, inputIntent, RuntimePredictionState::LocalPredictionStep
            );
            m_runtime.prediction.localSimAccumulator -= RuntimePredictionState::LocalPredictionStep;
            m_runtime.prediction.renderPrevSimState = m_runtime.prediction.renderCurrSimState;
            m_runtime.prediction.renderCurrSimState = m_runtime.gameplay.player->captureSimulationState();
            m_runtime.prediction.renderPrevPresentationState = m_runtime.prediction.renderCurrPresentationState;
            m_runtime.prediction.renderCurrPresentationState = m_runtime.gameplay.player->capturePresentationState();
            ++localPredictionSteps;
        }
        if (localPredictionSteps == 0) {
            // Keep player's network-input state synchronized even if no fixed-step sim ran.
            m_clientPrediction.update(m_runtime, inputIntent, 0.0);
        }
        if (localPredictionSteps == RuntimePredictionState::MaxLocalPredictionStepsPerFrame &&
            m_runtime.prediction.localSimAccumulator >= RuntimePredictionState::LocalPredictionStep) {
            m_runtime.prediction.localSimAccumulator =
                std::fmod(m_runtime.prediction.localSimAccumulator, RuntimePredictionState::LocalPredictionStep);
        }
    } else {
        m_runtime.prediction.localSimAccumulator = 0.0;
        m_runtime.world.renderStateNeedsResync = false;
        const Player::SimulationState frozenState = m_runtime.gameplay.player->captureSimulationState();
        const Player::PresentationState frozenPresentationState =
            m_runtime.gameplay.player->capturePresentationState();
        m_runtime.prediction.renderPrevSimState = frozenState;
        m_runtime.prediction.renderCurrSimState = frozenState;
        m_runtime.prediction.renderPrevPresentationState = frozenPresentationState;
        m_runtime.prediction.renderCurrPresentationState = frozenPresentationState;
    }
    const auto perfPredictionEnd = Clock::now();
    m_runtime.app.perf.predictionMs = MeasureMs(perfPredictionStart, perfPredictionEnd);

    const auto perfGameplayStart = Clock::now();
    WorldInteractionSystemContext worldInteractionCtx{};
    worldInteractionCtx.wasWorldInteractPressed = m_context.simulation.wasWorldInteractPressed;
    m_worldInteractionSystem.update(m_runtime, worldInteractionCtx);
    m_runtime.gameplay.player->updateRemotePlayers(static_cast<float>(GameData::deltaTime));
    CombatShootSystemContext combatShootCtx{};
    combatShootCtx.useDebugCamera =
        (m_context.simulation.useDebugCamera != nullptr) && *m_context.simulation.useDebugCamera;
    m_combatShootSystem.update(m_runtime, combatShootCtx);
    const auto perfGameplayEnd = Clock::now();
    m_runtime.app.perf.gameplayMs = MeasureMs(perfGameplayStart, perfGameplayEnd);

    SimulationStageResult result;
    result.localPredictionSteps = localPredictionSteps;
    result.renderCapabilities = m_runtime.render.renderer->getCapabilities();
    return result;
}

FrameOrchestrator::UiStageResult
FrameOrchestrator::runUiStage(const SimulationStageResult &simulation) {
    UiStageResult result;
    if (!m_runtime.ui.debugUi) {
        return result;
    }

    m_runtime.ui.debugUi->drawCrosshair(!GameData::cursorEnabled && m_runtime.combat.localPlayerAlive);
    if (m_runtime.ui.debugUi->isVisible()) {
        const ClientNetwork::ChunkQueueDepths queueDepths =
            m_runtime.network.clientNet.GetChunkQueueDepths();
        UiFrameData frameData;
        frameData.fps =
            (GameData::deltaTime > 1e-6) ? static_cast<float>(1.0 / GameData::deltaTime) : 0.0f;
        frameData.frameMs = static_cast<float>(GameData::deltaTime * 1000.0);
        frameData.playerPosition = m_runtime.gameplay.player->getPosition();
        frameData.playerVelocity = m_runtime.gameplay.player->getVelocity();
        frameData.flyMode = m_runtime.gameplay.player->flyMode;
        frameData.onGround = m_runtime.gameplay.player->isGrounded();
        frameData.renderDistance = m_runtime.gameplay.player->renderDistance;
        frameData.remotePlayerCount = m_runtime.gameplay.player->connectedPlayers.size();
        frameData.netConnected = m_runtime.network.clientNet.IsConnected();
        frameData.netStatus = m_runtime.network.clientNet.GetConnectionStatusText();
        frameData.serverTick = m_runtime.prediction.lastAppliedServerTick;
        frameData.ackedInputTick = m_runtime.prediction.lastAckedInputTick;
        frameData.pendingInputCount = m_runtime.prediction.pendingInputs.size();
        frameData.chunkDataQueueDepth = queueDepths.chunkData;
        frameData.chunkDeltaQueueDepth = queueDepths.chunkDelta;
        frameData.chunkUnloadQueueDepth = queueDepths.chunkUnload;
        frameData.backendName = simulation.renderCapabilities.backendName;
        frameData.mdiUsable = simulation.renderCapabilities.mdiUsable;
        frameData.perfFrameCpuMs = m_runtime.app.perf.frameCpuMs;
        frameData.perfInputMs = m_runtime.app.perf.inputMs;
        frameData.perfNetworkMs = m_runtime.app.perf.networkMs;
        frameData.perfPredictionMs = m_runtime.app.perf.predictionMs;
        frameData.perfGameplayMs = m_runtime.app.perf.gameplayMs;
        frameData.perfRenderCpuMs = m_runtime.app.perf.renderCpuMs;
        frameData.perfPresentMs = m_runtime.app.perf.presentMs;
        frameData.perfChunkStreamingMs = m_runtime.app.perf.chunkStreamingMs;
        m_runtime.render.renderer->appendBackendDebugUiFrameData(frameData);

        UiMutableState mutableState;
        mutableState.useDebugCamera = m_context.simulation.useDebugCamera;
        mutableState.toggleWireframe = m_context.render.toggleWireframe;
        mutableState.toggleChunkBorders = m_context.render.toggleChunkBorders;
        mutableState.toggleDebugFrustum = m_context.render.toggleDebugFrustum;
        mutableState.renderDistance = &m_runtime.gameplay.player->renderDistance;
        mutableState.cursorEnabled = &GameData::cursorEnabled;
        mutableState.rawMouseInputEnabled = m_context.ui.enableRawMouseInput;
        mutableState.rawMouseInputSupported = true;
        mutableState.gunViewOffset = &m_runtime.combat.equippedGunViewOffset;
        mutableState.gunViewScale = &m_runtime.combat.equippedGunViewScale;
        mutableState.gunViewEulerDeg = &m_runtime.combat.equippedGunViewEulerDeg;
        mutableState.sunDirection = m_context.render.sunDirection;
        mutableState.sunShadowDirectionalBias = m_context.render.sunShadowDirectionalBias;
        mutableState.sunShadowLowSunBiasBoost = m_context.render.sunShadowLowSunBiasBoost;
        mutableState.sunShadowFrontFaceCullAtLowSun = m_context.render.sunShadowFrontFaceCullAtLowSun;
        mutableState.sunShadowFrontFaceCullGrazingThreshold =
            m_context.render.sunShadowFrontFaceCullGrazingThreshold;
        mutableState.skyExposure = m_context.render.skyExposure;
        mutableState.giTracingBackendPreference = simulation.renderCapabilities.supportsGiRuntimeControls
                                                      ? &GameData::giTracingBackendPreference
                                                      : nullptr;
        mutableState.giNrdDebugView =
            simulation.renderCapabilities.supportsGiRuntimeControls ? &GameData::giNrdDebugView : nullptr;
        mutableState.giNrdGuideOverride =
            simulation.renderCapabilities.supportsGiRuntimeControls
                ? &GameData::giNrdGuideOverride
                : nullptr;

        m_runtime.ui.debugUi->drawMainWindow(frameData, mutableState);
    }

    if (!m_runtime.ui.debugUi->isVisible() && m_context.ui.showDebugUi != nullptr &&
        *m_context.ui.showDebugUi) {
        *m_context.ui.showDebugUi = false;
    }
    if (m_runtime.ui.inventoryUi) {
        m_runtime.ui.inventoryUi->draw(m_runtime.network.clientNet, m_runtime.network.clientNet.IsConnected());
        if (!m_runtime.ui.inventoryUi->isVisible() && m_context.ui.showInventoryUi != nullptr &&
            *m_context.ui.showInventoryUi) {
            *m_context.ui.showInventoryUi = false;
        }
    }

    const bool forceCursor = (m_context.simulation.forceCursorEnabled != nullptr) &&
                             *m_context.simulation.forceCursorEnabled;
    const bool showDebugUi = (m_context.ui.showDebugUi != nullptr) && *m_context.ui.showDebugUi;
    const bool showInventoryUi =
        (m_context.ui.showInventoryUi != nullptr) && *m_context.ui.showInventoryUi;
    GameData::cursorEnabled =
        forceCursor || showDebugUi || showInventoryUi || !m_runtime.network.clientNet.IsConnected();
    if (m_context.host.applyMouseInputModes) {
        m_context.host.applyMouseInputModes();
    }

    HudContext hudCtx{};
    hudCtx.window = m_context.host.window;
    hudCtx.serverIp = m_context.ui.serverIp;
    hudCtx.serverPort = m_context.ui.serverPort;
    hudCtx.requestedUsername = m_context.ui.requestedUsername;
    hudCtx.beginConnectionAttempt = m_context.simulation.beginConnectionAttempt;
    hudCtx.applyMouseInputModes = m_context.host.applyMouseInputModes;
    m_hudSystem.draw(m_runtime, hudCtx);
    result.drawData = m_runtime.ui.debugUi->endFrame();
    return result;
}

void FrameOrchestrator::runRenderStage(
    const SimulationStageResult &simulation, const UiStageResult &ui
) {
    const bool useDebugCamera =
        (m_context.simulation.useDebugCamera != nullptr) && *m_context.simulation.useDebugCamera;
    const glm::vec3 contextSunDirection = *m_context.render.sunDirection;
    RenderSceneBuilderInput sceneInput{};
    sceneInput.useDebugCamera = useDebugCamera;
    sceneInput.toggleWireframe =
        (m_context.render.toggleWireframe != nullptr) && *m_context.render.toggleWireframe;
    sceneInput.toggleChunkBorders =
        (m_context.render.toggleChunkBorders != nullptr) && *m_context.render.toggleChunkBorders;
    sceneInput.toggleDebugFrustum =
        (m_context.render.toggleDebugFrustum != nullptr) && *m_context.render.toggleDebugFrustum;
    sceneInput.sunDirection = contextSunDirection;
    sceneInput.skyExposure = *m_context.render.skyExposure;
    sceneInput.sunShadowDirectionalBias = *m_context.render.sunShadowDirectionalBias;
    sceneInput.sunShadowLowSunBiasBoost = *m_context.render.sunShadowLowSunBiasBoost;
    sceneInput.sunShadowFrontFaceCullAtLowSun = *m_context.render.sunShadowFrontFaceCullAtLowSun;
    sceneInput.sunShadowFrontFaceCullGrazingThreshold =
        *m_context.render.sunShadowFrontFaceCullGrazingThreshold;
    sceneInput.uiDrawData = ui.drawData;

    RenderScene frameScene = m_renderSceneBuilder.build(m_runtime, sceneInput);
    frameScene.renderOpaqueOverlayPasses = [&]() {
        if (m_context.render.renderWorldItems) {
            m_context.render.renderWorldItems(m_runtime, frameScene.activeCamera);
        }
        if (frameScene.useDebugCamera && m_runtime.render.gunSceneRenderer) {
            m_runtime.render.gunSceneRenderer->renderRemotePlayerGuns(
                m_runtime, frameScene.activeCamera, frameScene.sunDirection
            );
        }
    };
    m_runtime.render.renderer->renderFrame(frameScene);
    *m_context.render.sunDirection = frameScene.sunDirection;

    if (simulation.renderCapabilities.supportsFirstPersonViewmodel && !useDebugCamera &&
        m_runtime.combat.localPlayerAlive && m_runtime.render.gunSceneRenderer) {
        m_runtime.render.gunSceneRenderer->renderHeldGun(
            m_runtime, m_runtime.render.interpolatedPlayerCamera, frameScene.sunDirection
        );
    }
    if (m_runtime.ui.debugUi && !simulation.renderCapabilities.compositesUiInRenderFrame) {
        m_runtime.ui.debugUi->renderDrawData(ui.drawData);
    }
}

void FrameOrchestrator::updateFrameHotkeysAndCounters() {
    static bool f11PressedLastFrame = false;
    const bool f11PressedNow = IsScancodeDown(SDL_SCANCODE_F11);
    if (f11PressedNow && !f11PressedLastFrame) {
        if (m_context.host.toggleFullscreen) {
            m_context.host.toggleFullscreen(m_context.host.window);
        }
    }
    f11PressedLastFrame = f11PressedNow;
    if (m_context.host.updateFPSCounter) {
        m_context.host.updateFPSCounter();
    }
}

void FrameOrchestrator::runPresentStage(size_t localPredictionSteps) {
    const auto perfPresentStart = Clock::now();
    m_runtime.render.renderer->present(m_context.host.window);
    if (m_context.host.pollEvents) {
        m_context.host.pollEvents(m_runtime);
    }
    const auto perfPresentEnd = Clock::now();
    m_runtime.app.perf.presentMs = MeasureMs(perfPresentStart, perfPresentEnd);

    const ClientNetwork::ChunkQueueDepths queueDepths = m_runtime.network.clientNet.GetChunkQueueDepths();
    const bool frameUnderPressure = GameData::deltaTime > (RuntimePredictionState::LocalPredictionStep * 1.2);
    const bool chunkBacklog =
        queueDepths.chunkData > (RuntimeWorldState::MaxChunkDataApplyPerFrame * 3) ||
        queueDepths.chunkDelta > (RuntimeWorldState::MaxChunkDeltaApplyPerFrame * 3) ||
        queueDepths.chunkUnload > (RuntimeWorldState::MaxChunkUnloadApplyPerFrame * 3);
    const bool prioritizeMovement =
        (localPredictionSteps > 1) || frameUnderPressure || chunkBacklog;
    const auto perfChunkStart = Clock::now();
    m_chunkStreaming.update(m_runtime, prioritizeMovement);
    const auto perfChunkEnd = Clock::now();
    m_runtime.app.perf.chunkStreamingMs = MeasureMs(perfChunkStart, perfChunkEnd);
}






