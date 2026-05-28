#include "FrameOrchestrator.hpp"

#include "AppHelpers.hpp"
#include "../graphics/IGunSceneRenderer.hpp"
#include "../graphics/Camera.hpp"
#include "../render/RenderScene.hpp"
#include "../data/GameData.hpp"
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>

#include <glm/geometric.hpp>

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

    glm::vec3 ComputeViewmodelMuzzlePosition(const Camera &camera, const RuntimeCombatState &combat) {
        glm::vec3 forward = camera.front;
        const float forwardLenSq = glm::dot(forward, forward);
        if (!std::isfinite(forwardLenSq) || forwardLenSq < 1e-8f) {
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        } else {
            forward = glm::normalize(forward);
        }

        glm::vec3 up = camera.up;
        const float upLenSq = glm::dot(up, up);
        if (!std::isfinite(upLenSq) || upLenSq < 1e-8f) {
            up = glm::vec3(0.0f, 1.0f, 0.0f);
        } else {
            up = glm::normalize(up);
        }

        glm::vec3 right = glm::cross(forward, up);
        const float rightLenSq = glm::dot(right, right);
        if (!std::isfinite(rightLenSq) || rightLenSq < 1e-8f) {
            right = glm::vec3(1.0f, 0.0f, 0.0f);
        } else {
            right = glm::normalize(right);
        }
        up = glm::normalize(glm::cross(right, forward));

        const glm::vec3 gunPos = camera.position + right * combat.equippedGunViewOffset.x +
                                 up * combat.equippedGunViewOffset.y +
                                 forward * combat.equippedGunViewOffset.z;
        return gunPos + (forward * 0.70f);
    }

    void AssertRequiredContext(const FrameOrchestratorContext &ctx) {
        assert(ctx.inputHost != nullptr);
        assert(ctx.connectionHost != nullptr);
        assert(ctx.windowHost != nullptr);
        assert(ctx.renderHost != nullptr);
        assert(ctx.host.window != nullptr);

        assert(ctx.simulation.useDebugCamera != nullptr);
        assert(ctx.simulation.forceCursorEnabled != nullptr);
        assert(ctx.simulation.wasWorldInteractPressed != nullptr);

        assert(ctx.ui.serverIp != nullptr);
        assert(ctx.ui.serverPort != nullptr);
        assert(ctx.ui.requestedUsername != nullptr);
        assert(ctx.ui.showDebugUi != nullptr);
        assert(ctx.ui.showInventoryUi != nullptr);
        assert(ctx.ui.enableRawMouseInput != nullptr);
        assert(ctx.ui.requestSwitchToOpenGl != nullptr);
        assert(ctx.ui.requestSwitchToVulkan != nullptr);
        assert(ctx.ui.renderApiPreference != nullptr);

        assert(ctx.render.skyExposure != nullptr);
        assert(ctx.render.sunDirection != nullptr);
        assert(ctx.render.sunShadowDirectionalBias != nullptr);
        assert(ctx.render.sunShadowLowSunBiasBoost != nullptr);
        assert(ctx.render.sunShadowFrontFaceCullAtLowSun != nullptr);
        assert(ctx.render.sunShadowFrontFaceCullGrazingThreshold != nullptr);
    }
} // namespace

void FrameOrchestrator::bind(Runtime &runtime, FrameOrchestratorContext context) {
    AssertRequiredContext(context);
    m_runtime = &runtime;
    m_context = std::move(context);
}

Runtime &FrameOrchestrator::runtime() {
    assert(m_runtime != nullptr);
    return *m_runtime;
}

const Runtime &FrameOrchestrator::runtime() const {
    assert(m_runtime != nullptr);
    return *m_runtime;
}


void FrameOrchestrator::runFrame() {
    assert(m_runtime != nullptr);

    const auto perfFrameStart = Clock::now();
    updateFrameTime();

    const SimulationStageResult simulation = runSimulationStage();

    const auto perfRenderStart = Clock::now();
    const UiStageResult ui = runUiStage(simulation);
    runRenderStage(simulation, ui);
    updateFrameHotkeysAndCounters();
    const auto perfRenderEnd = Clock::now();
    runtime().app.perf.renderCpuMs = MeasureMs(perfRenderStart, perfRenderEnd);

    runPresentStage(simulation.localPredictionSteps);

    const auto perfFrameEnd = Clock::now();
    runtime().app.perf.frameCpuMs = MeasureMs(perfFrameStart, perfFrameEnd);
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
            runtime().network.clientNet.GetChunkQueueDepths();
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
    FrameInputHost *inputHost = m_context.inputHost;
    FrameConnectionHost *connectionHost = m_context.connectionHost;
    FrameWindowHost *windowHost = m_context.windowHost;

    const auto perfInputStart = Clock::now();
    if (runtime().ui.debugUi) {
        runtime().ui.debugUi->beginFrame();
    }
    if (inputHost != nullptr) {
        inputHost->updateDebugCamera(runtime());
    }
    m_uiStateController.update(runtime().ui, runtime().network.clientNet);
    if (inputHost != nullptr) {
        inputHost->updateToggleStates(runtime());
    }
    GameData::gameplayInputEnabled =
        runtime().network.clientNet.IsConnected() &&
        (runtime().ui.activeView == UiView::InGame) && (m_context.ui.showDebugUi != nullptr) &&
        (m_context.ui.showInventoryUi != nullptr) &&
        (m_context.simulation.forceCursorEnabled != nullptr) && !*m_context.ui.showDebugUi &&
        !*m_context.ui.showInventoryUi && !*m_context.simulation.forceCursorEnabled;

    runtime().gameplay.inputCallbacks->processInput(m_context.host.window);
    if (windowHost != nullptr) {
        windowHost->applyMouseInputModes();
    }
    const ClientInputIntent inputIntent = m_inputSystem.captureIntent(runtime(), m_context.host.window);
    runtime().prediction.localSimAccumulator += GameData::deltaTime;
    const auto perfInputEnd = Clock::now();
    runtime().app.perf.inputMs = MeasureMs(perfInputStart, perfInputEnd);

    const double maxAccumulatedTime = RuntimePredictionState::LocalPredictionStep *
                                      static_cast<double>(RuntimePredictionState::MaxLocalPredictionStepsPerFrame);
    if (runtime().prediction.localSimAccumulator > maxAccumulatedTime) {
        runtime().prediction.localSimAccumulator = maxAccumulatedTime;
    }

    const auto perfNetworkStart = Clock::now();
    ClientSessionContext netCtx{};
    netCtx.forceCursorEnabled = m_context.simulation.forceCursorEnabled;
    netCtx.connectionHost = connectionHost;
    m_clientSession.update(runtime(), netCtx, &inputIntent);
    const auto perfNetworkEnd = Clock::now();
    runtime().app.perf.networkMs = MeasureMs(perfNetworkStart, perfNetworkEnd);

    const auto perfPredictionStart = Clock::now();
    if (!runtime().prediction.hasRenderSimState) {
        const Player::SimulationState initialSimState = runtime().gameplay.player->captureSimulationState();
        const Player::PresentationState initialPresentationState =
            runtime().gameplay.player->capturePresentationState();
        runtime().prediction.renderPrevSimState = initialSimState;
        runtime().prediction.renderCurrSimState = initialSimState;
        runtime().prediction.renderPrevPresentationState = initialPresentationState;
        runtime().prediction.renderCurrPresentationState = initialPresentationState;
        runtime().prediction.hasRenderSimState = true;
    }

    size_t localPredictionSteps = 0;
    if (runtime().combat.localPlayerAlive) {
        while (runtime().prediction.localSimAccumulator >= RuntimePredictionState::LocalPredictionStep &&
               localPredictionSteps < RuntimePredictionState::MaxLocalPredictionStepsPerFrame) {
            m_clientPrediction.update(
                runtime(), inputIntent, RuntimePredictionState::LocalPredictionStep
            );
            runtime().prediction.localSimAccumulator -= RuntimePredictionState::LocalPredictionStep;
            runtime().prediction.renderPrevSimState = runtime().prediction.renderCurrSimState;
            runtime().prediction.renderCurrSimState = runtime().gameplay.player->captureSimulationState();
            runtime().prediction.renderPrevPresentationState = runtime().prediction.renderCurrPresentationState;
            runtime().prediction.renderCurrPresentationState = runtime().gameplay.player->capturePresentationState();
            ++localPredictionSteps;
        }
        if (localPredictionSteps == 0) {
            // Keep player's network-input state synchronized even if no fixed-step sim ran.
            m_clientPrediction.update(runtime(), inputIntent, 0.0);
        }
        if (localPredictionSteps == RuntimePredictionState::MaxLocalPredictionStepsPerFrame &&
            runtime().prediction.localSimAccumulator >= RuntimePredictionState::LocalPredictionStep) {
            runtime().prediction.localSimAccumulator =
                std::fmod(runtime().prediction.localSimAccumulator, RuntimePredictionState::LocalPredictionStep);
        }
    } else {
        runtime().prediction.localSimAccumulator = 0.0;
        runtime().world.renderStateNeedsResync = false;
        const Player::SimulationState frozenState = runtime().gameplay.player->captureSimulationState();
        const Player::PresentationState frozenPresentationState =
            runtime().gameplay.player->capturePresentationState();
        runtime().prediction.renderPrevSimState = frozenState;
        runtime().prediction.renderCurrSimState = frozenState;
        runtime().prediction.renderPrevPresentationState = frozenPresentationState;
        runtime().prediction.renderCurrPresentationState = frozenPresentationState;
    }
    const auto perfPredictionEnd = Clock::now();
    runtime().app.perf.predictionMs = MeasureMs(perfPredictionStart, perfPredictionEnd);

    const auto perfGameplayStart = Clock::now();
    WorldInteractionSystemContext worldInteractionCtx{};
    worldInteractionCtx.wasWorldInteractPressed = m_context.simulation.wasWorldInteractPressed;
    m_worldInteractionSystem.update(runtime(), worldInteractionCtx);
    runtime().gameplay.player->updateRemotePlayers(static_cast<float>(GameData::deltaTime));
    CombatShootSystemContext combatShootCtx{};
    combatShootCtx.useDebugCamera =
        (m_context.simulation.useDebugCamera != nullptr) && *m_context.simulation.useDebugCamera;
    m_combatShootSystem.update(runtime(), combatShootCtx);
    CombatGrappleSystemContext combatGrappleCtx{};
    combatGrappleCtx.useDebugCamera = combatShootCtx.useDebugCamera;
    m_combatGrappleSystem.update(runtime(), combatGrappleCtx);
    const auto perfGameplayEnd = Clock::now();
    runtime().app.perf.gameplayMs = MeasureMs(perfGameplayStart, perfGameplayEnd);

    SimulationStageResult result;
    result.localPredictionSteps = localPredictionSteps;
    result.renderCapabilities = runtime().render.renderer->getCapabilities();
    return result;
}




FrameOrchestrator::UiStageResult
FrameOrchestrator::runUiStage(const SimulationStageResult &simulation) {
    UiStageResult result;
    GameData::uiWantsMouseCapture = false;
    GameData::uiWantsKeyboardCapture = false;
    GameData::uiWantsTextInput = false;

    if (runtime().ui.rmlUi) {
        GameData::uiWantsMouseCapture = runtime().ui.rmlUi->wantsMouseCapture();
        GameData::uiWantsKeyboardCapture = runtime().ui.rmlUi->wantsKeyboardCapture();
        GameData::uiWantsTextInput = runtime().ui.rmlUi->wantsKeyboardCapture();
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        const ImGuiIO &io = ImGui::GetIO();
        GameData::uiWantsMouseCapture = GameData::uiWantsMouseCapture || io.WantCaptureMouse;
        GameData::uiWantsKeyboardCapture = GameData::uiWantsKeyboardCapture || io.WantCaptureKeyboard;
        GameData::uiWantsTextInput = GameData::uiWantsTextInput || io.WantTextInput;
    }

    if (runtime().ui.debugUi) {
        const bool rmlOwnsHud = runtime().ui.rmlUi && runtime().ui.rmlUi->isUsingOpenGlBackend();
        if (!rmlOwnsHud) {
            runtime().ui.debugUi->drawCrosshair(
                !GameData::cursorEnabled && runtime().combat.localPlayerAlive
            );
        }
    }
    if (runtime().ui.debugUi && runtime().ui.debugUi->isVisible()) {
        const ClientNetwork::ChunkQueueDepths queueDepths =
            runtime().network.clientNet.GetChunkQueueDepths();
        UiFrameData frameData;
        frameData.fps =
            (GameData::deltaTime > 1e-6) ? static_cast<float>(1.0 / GameData::deltaTime) : 0.0f;
        frameData.frameMs = static_cast<float>(GameData::deltaTime * 1000.0);
        frameData.playerPosition = runtime().gameplay.player->getPosition();
        frameData.playerVelocity = runtime().gameplay.player->getVelocity();
        frameData.flyMode = runtime().gameplay.player->flyMode;
        frameData.onGround = runtime().gameplay.player->isGrounded();
        frameData.renderDistance = runtime().gameplay.player->renderDistance;
        frameData.remotePlayerCount = runtime().gameplay.player->connectedPlayers.size();
        frameData.netConnected = runtime().network.clientNet.IsConnected();
        frameData.netStatus = runtime().network.clientNet.GetConnectionStatusText();
        frameData.serverTick = runtime().prediction.lastAppliedServerTick;
        frameData.ackedInputTick = runtime().prediction.lastAckedInputTick;
        frameData.pendingInputCount = runtime().prediction.pendingInputs.size();
        frameData.chunkDataQueueDepth = queueDepths.chunkData;
        frameData.chunkDeltaQueueDepth = queueDepths.chunkDelta;
        frameData.chunkUnloadQueueDepth = queueDepths.chunkUnload;
        frameData.backendName = simulation.renderCapabilities.backendName;
        frameData.mdiUsable = simulation.renderCapabilities.mdiUsable;
        frameData.perfFrameCpuMs = runtime().app.perf.frameCpuMs;
        frameData.perfInputMs = runtime().app.perf.inputMs;
        frameData.perfNetworkMs = runtime().app.perf.networkMs;
        frameData.perfPredictionMs = runtime().app.perf.predictionMs;
        frameData.perfGameplayMs = runtime().app.perf.gameplayMs;
        frameData.perfRenderCpuMs = runtime().app.perf.renderCpuMs;
        frameData.perfPresentMs = runtime().app.perf.presentMs;
        frameData.perfChunkStreamingMs = runtime().app.perf.chunkStreamingMs;
        runtime().render.renderer->appendBackendDebugUiFrameData(frameData);

        UiMutableState mutableState;
        mutableState.useDebugCamera = m_context.simulation.useDebugCamera;
        mutableState.toggleWireframe = m_context.render.toggleWireframe;
        mutableState.toggleChunkBorders = m_context.render.toggleChunkBorders;
        mutableState.toggleDebugFrustum = m_context.render.toggleDebugFrustum;
        mutableState.renderDistance = &runtime().gameplay.player->renderDistance;
        mutableState.cursorEnabled = &GameData::cursorEnabled;
        mutableState.rawMouseInputEnabled = m_context.ui.enableRawMouseInput;
        mutableState.rawMouseInputSupported = true;
        mutableState.gunViewOffset = &runtime().combat.equippedGunViewOffset;
        mutableState.gunViewScale = &runtime().combat.equippedGunViewScale;
        mutableState.gunViewEulerDeg = &runtime().combat.equippedGunViewEulerDeg;
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
        mutableState.isVulkanActive = simulation.renderCapabilities.api == RenderApi::Vulkan;
        mutableState.isOpenGlActive = simulation.renderCapabilities.api == RenderApi::OpenGL;

        mutableState.requestSwitchToOpenGl = m_context.ui.requestSwitchToOpenGl;
        mutableState.requestSwitchToVulkan = m_context.ui.requestSwitchToVulkan;
        mutableState.renderApiPreference = m_context.ui.renderApiPreference;

        runtime().ui.debugUi->drawMainWindow(frameData, mutableState);
    }

    if (runtime().ui.debugUi && !runtime().ui.debugUi->isVisible() && m_context.ui.showDebugUi != nullptr &&
        *m_context.ui.showDebugUi) {
        *m_context.ui.showDebugUi = false;
    }
    if (runtime().ui.inventoryUi) {
        runtime().ui.inventoryUi->draw(
            runtime(), runtime().network.clientNet, runtime().network.clientNet.IsConnected()
        );
        if (!runtime().ui.inventoryUi->isVisible() && m_context.ui.showInventoryUi != nullptr &&
            *m_context.ui.showInventoryUi) {
            *m_context.ui.showInventoryUi = false;
        }
    }

    m_uiStateController.update(runtime().ui, runtime().network.clientNet);
    if (runtime().ui.activeView == UiView::MainMenu) {
        MainMenuContext mainMenuCtx{};
        mainMenuCtx.window = m_context.host.window;
        mainMenuCtx.serverIp = m_context.ui.serverIp;
        mainMenuCtx.serverPort = m_context.ui.serverPort;
        mainMenuCtx.requestedUsername = m_context.ui.requestedUsername;
        mainMenuCtx.connectionHost = m_context.connectionHost;
        mainMenuCtx.windowHost = m_context.windowHost;
        m_mainMenu.draw(runtime(), mainMenuCtx);
    } else {
        m_mainMenu.hide();
    }
    const bool forceCursor = (m_context.simulation.forceCursorEnabled != nullptr) &&
                             *m_context.simulation.forceCursorEnabled;
    const bool showDebugUi = (m_context.ui.showDebugUi != nullptr) && *m_context.ui.showDebugUi;
    const bool showInventoryUi =
        (m_context.ui.showInventoryUi != nullptr) && *m_context.ui.showInventoryUi;
    GameData::cursorEnabled = forceCursor || showDebugUi || showInventoryUi || runtime().ui.wantsCursor;
    if (!GameData::cursorEnabled && runtime().ui.activeView == UiView::InGame) {
        GameData::uiWantsMouseCapture = false;
    }
    if (m_context.windowHost != nullptr) {
        m_context.windowHost->applyMouseInputModes();
    }

    m_hudSystem.draw(runtime());
    if (runtime().ui.rmlUi) {
        runtime().ui.rmlUi->update();
    }
    result.drawData = runtime().ui.debugUi ? runtime().ui.debugUi->endFrame() : nullptr;
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

    RenderScene frameScene = m_renderSceneBuilder.build(runtime(), sceneInput);
    const bool renderDebugLine = simulation.renderCapabilities.api == RenderApi::OpenGL;
    const bool renderGrappleRope =
        renderDebugLine && runtime().combat.grapple.isAttached && runtime().combat.localPlayerAlive;
    constexpr double kAcceptedShotLineDurationSeconds = 0.30;
    constexpr double kLocalShotFallbackDurationSeconds = 0.10;
    const bool acceptedMatchesLocalShot =
        runtime().combat.hasLastAcceptedShotResult && runtime().combat.hasLastLocalShot &&
        (runtime().combat.lastAcceptedShotResultId == runtime().combat.lastLocalShotId);
    const bool drawAcceptedShotLine =
        renderDebugLine && acceptedMatchesLocalShot &&
        ((m_frameNow - runtime().combat.lastAcceptedShotResultTime) <=
         kAcceptedShotLineDurationSeconds);
    const bool drawLocalShotFallback =
        renderDebugLine && runtime().combat.hasLastLocalShot &&
        ((m_frameNow - runtime().combat.lastLocalShotTime) <= kLocalShotFallbackDurationSeconds);
    frameScene.renderOpaqueOverlayPasses = [&]() {
        if (renderGrappleRope) {
            const glm::vec3 ropeStart =
                ComputeViewmodelMuzzlePosition(frameScene.activeCamera, runtime().combat);
            m_debugRenderer.drawLine(
                ropeStart,
                runtime().combat.grapple.anchorPoint,
                glm::vec3(0.35f, 0.95f, 1.0f),
                LineDrawMode::TwoVertices,
                1.0f,
                frameScene.activeCamera
            );
        }
        if (drawAcceptedShotLine) {
            m_debugRenderer.drawLine(
                runtime().combat.lastLocalShotOrigin,
                runtime().combat.lastAcceptedShotResultPoint,
                glm::vec3(1.0f, 0.85f, 0.15f),
                LineDrawMode::TwoVertices,
                1.0f,
                frameScene.activeCamera
            );
        } else if (drawLocalShotFallback) {
            m_debugRenderer.drawLine(
                runtime().combat.lastLocalShotOrigin,
                runtime().combat.lastLocalShotOrigin +
                    (runtime().combat.lastLocalShotDirection * 64.0f),
                glm::vec3(1.0f, 0.45f, 0.1f),
                LineDrawMode::TwoVertices,
                1.0f,
                frameScene.activeCamera
            );
        }
        if (m_context.renderHost != nullptr) {
            m_context.renderHost->renderWorldItems(runtime(), frameScene.activeCamera);
        }
        if (frameScene.useDebugCamera && runtime().render.gunSceneRenderer) {
            runtime().render.gunSceneRenderer->renderRemotePlayerGuns(
                runtime(), frameScene.activeCamera, frameScene.sunDirection
            );
        }
    };
    runtime().render.renderer->renderFrame(frameScene);
    *m_context.render.sunDirection = frameScene.sunDirection;

    if (runtime().ui.rmlUi && runtime().ui.rmlUi->isUsingOpenGlBackend() &&
        simulation.renderCapabilities.api == RenderApi::OpenGL) {
        runtime().ui.rmlUi->render();
    }

    if (simulation.renderCapabilities.supportsFirstPersonViewmodel && !useDebugCamera &&
        runtime().combat.localPlayerAlive && runtime().render.gunSceneRenderer) {
        runtime().render.gunSceneRenderer->renderHeldGun(
            runtime(), runtime().render.interpolatedPlayerCamera, frameScene.sunDirection
        );
    }
    if (runtime().ui.debugUi && !simulation.renderCapabilities.compositesUiInRenderFrame) {
        runtime().ui.debugUi->renderDrawData(ui.drawData);
    }
}

void FrameOrchestrator::updateFrameHotkeysAndCounters() {
    static bool f11PressedLastFrame = false;
    const bool f11PressedNow = IsScancodeDown(SDL_SCANCODE_F11);
    if (f11PressedNow && !f11PressedLastFrame) {
        if (m_context.windowHost != nullptr) {
            m_context.windowHost->toggleFullscreen(m_context.host.window);
        }
    }
    f11PressedLastFrame = f11PressedNow;
    if (m_context.windowHost != nullptr) {
        m_context.windowHost->updateFPSCounter();
    }
}

void FrameOrchestrator::runPresentStage(size_t localPredictionSteps) {
    const auto perfPresentStart = Clock::now();
    runtime().render.renderer->present(m_context.host.window);
    if (m_context.inputHost != nullptr) {
        m_context.inputHost->pollEvents(runtime());
    }
    const auto perfPresentEnd = Clock::now();
    runtime().app.perf.presentMs = MeasureMs(perfPresentStart, perfPresentEnd);

    const ClientNetwork::ChunkQueueDepths queueDepths = runtime().network.clientNet.GetChunkQueueDepths();
    const bool frameUnderPressure = GameData::deltaTime > (RuntimePredictionState::LocalPredictionStep * 1.2);
    const bool chunkBacklog =
        queueDepths.chunkData > (RuntimeWorldState::MaxChunkDataApplyPerFrame * 3) ||
        queueDepths.chunkDelta > (RuntimeWorldState::MaxChunkDeltaApplyPerFrame * 3) ||
        queueDepths.chunkUnload > (RuntimeWorldState::MaxChunkUnloadApplyPerFrame * 3);
    const bool prioritizeMovement =
        (localPredictionSteps > 1) || frameUnderPressure || chunkBacklog;
    const auto perfChunkStart = Clock::now();
    m_chunkStreaming.update(runtime(), prioritizeMovement);
    const auto perfChunkEnd = Clock::now();
    runtime().app.perf.chunkStreamingMs = MeasureMs(perfChunkStart, perfChunkEnd);
}






