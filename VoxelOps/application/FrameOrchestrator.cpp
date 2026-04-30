#include "FrameOrchestrator.hpp"

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RenderFrameParams.hpp"
#include "../graphics/IGunSceneRenderer.hpp"
#include "../data/GameData.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;

float MeasureMs(const Clock::time_point &start, const Clock::time_point &end) {
    return static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                                  .count()) *
           0.001f;
}

bool IsScancodeDown(SDL_Scancode scancode) {
    int keyCount = 0;
    const bool *keys = SDL_GetKeyboardState(&keyCount);
    return keys != nullptr && scancode < keyCount && keys[scancode];
}
} // namespace

FrameOrchestrator::FrameOrchestrator(App &app, Runtime &runtime) : m_app(app), m_runtime(runtime) {}

void FrameOrchestrator::runFrame() {
    const auto perfFrameStart = Clock::now();
    updateFrameTime();

    const SimulationStageResult simulation = runSimulationStage();

    const auto perfRenderStart = Clock::now();
    const UiStageResult ui = runUiStage(simulation);
    runRenderStage(simulation, ui);
    updateFrameHotkeysAndCounters();
    const auto perfRenderEnd = Clock::now();
    m_runtime.perf.renderCpuMs = MeasureMs(perfRenderStart, perfRenderEnd);

    runPresentStage(simulation.localPredictionSteps);

    const auto perfFrameEnd = Clock::now();
    m_runtime.perf.frameCpuMs = MeasureMs(perfFrameStart, perfFrameEnd);
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
        const ClientNetwork::ChunkQueueDepths queueDepths = m_runtime.clientNet.GetChunkQueueDepths();
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
    if (m_runtime.debugUi) {
        m_runtime.debugUi->beginFrame();
    }
    m_app.updateDebugCamera(m_runtime);
    m_app.updateToggleStates(m_runtime);

    if (!m_runtime.clientNet.IsConnected()) {
        GameData::cursorEnabled = true;
    }
    GameData::gameplayInputEnabled =
        m_runtime.clientNet.IsConnected() && !m_app.m_ShowDebugUi && !m_app.m_ForceCursorEnabled;

    m_runtime.inputCallbacks->processInput(m_app.m_Window);
    m_app.applyMouseInputModes();
    m_runtime.localSimAccumulator += GameData::deltaTime;
    const auto perfInputEnd = Clock::now();
    m_runtime.perf.inputMs = MeasureMs(perfInputStart, perfInputEnd);

    const double maxAccumulatedTime =
        Runtime::LocalPredictionStep * static_cast<double>(Runtime::MaxLocalPredictionStepsPerFrame);
    if (m_runtime.localSimAccumulator > maxAccumulatedTime) {
        m_runtime.localSimAccumulator = maxAccumulatedTime;
    }

    const auto perfNetworkStart = Clock::now();
    ClientNetworkSystemContext netCtx{};
    netCtx.window = m_app.m_Window;
    netCtx.forceCursorEnabled = &m_app.m_ForceCursorEnabled;
    netCtx.beginConnectionAttempt = [this](Runtime &runtime) {
        return m_app.beginConnectionAttempt(runtime);
    };
    netCtx.equipGun = [this](Runtime &runtime, GunType gunType) {
        return m_app.equipGun(runtime, gunType);
    };
    m_networkSystem.update(m_runtime, netCtx);
    const auto perfNetworkEnd = Clock::now();
    m_runtime.perf.networkMs = MeasureMs(perfNetworkStart, perfNetworkEnd);

    const auto perfPredictionStart = Clock::now();
    if (!m_runtime.hasRenderSimState) {
        const Player::SimulationState initialSimState = m_runtime.player->captureSimulationState();
        m_runtime.renderPrevSimState = initialSimState;
        m_runtime.renderCurrSimState = initialSimState;
        m_runtime.hasRenderSimState = true;
    }

    size_t localPredictionSteps = 0;
    if (m_runtime.combat.localPlayerAlive) {
        while (m_runtime.localSimAccumulator >= Runtime::LocalPredictionStep &&
               localPredictionSteps < Runtime::MaxLocalPredictionStepsPerFrame) {
            m_runtime.player->update(m_app.m_Window, Runtime::LocalPredictionStep);
            m_runtime.localSimAccumulator -= Runtime::LocalPredictionStep;
            m_runtime.renderPrevSimState = m_runtime.renderCurrSimState;
            m_runtime.renderCurrSimState = m_runtime.player->captureSimulationState();
            ++localPredictionSteps;
        }
        if (localPredictionSteps == 0) {
            // Keep input sampling responsive even on very high FPS frames.
            m_runtime.player->update(m_app.m_Window, 0.0);
        }
        if (localPredictionSteps == Runtime::MaxLocalPredictionStepsPerFrame &&
            m_runtime.localSimAccumulator >= Runtime::LocalPredictionStep) {
            m_runtime.localSimAccumulator =
                std::fmod(m_runtime.localSimAccumulator, Runtime::LocalPredictionStep);
        }
    } else {
        m_runtime.localSimAccumulator = 0.0;
        m_runtime.renderStateNeedsResync = false;
        const Player::SimulationState frozenState = m_runtime.player->captureSimulationState();
        m_runtime.renderPrevSimState = frozenState;
        m_runtime.renderCurrSimState = frozenState;
    }
    const auto perfPredictionEnd = Clock::now();
    m_runtime.perf.predictionMs = MeasureMs(perfPredictionStart, perfPredictionEnd);

    const auto perfGameplayStart = Clock::now();
    m_app.processWorldInteraction(m_runtime);
    m_runtime.player->updateRemotePlayers(static_cast<float>(GameData::deltaTime));
    m_app.processShooting(m_runtime);
    const auto perfGameplayEnd = Clock::now();
    m_runtime.perf.gameplayMs = MeasureMs(perfGameplayStart, perfGameplayEnd);

    SimulationStageResult result;
    result.localPredictionSteps = localPredictionSteps;
    result.renderCaps = m_runtime.renderer->getCapabilities();
    return result;
}

FrameOrchestrator::UiStageResult
FrameOrchestrator::runUiStage(const SimulationStageResult &simulation) {
    UiStageResult result;
    if (!m_runtime.debugUi) {
        return result;
    }

    m_runtime.debugUi->drawCrosshair(!GameData::cursorEnabled && m_runtime.combat.localPlayerAlive);
    if (m_runtime.debugUi->isVisible()) {
        const ClientNetwork::ChunkQueueDepths queueDepths = m_runtime.clientNet.GetChunkQueueDepths();
        UiFrameData frameData;
        frameData.fps =
            (GameData::deltaTime > 1e-6) ? static_cast<float>(1.0 / GameData::deltaTime) : 0.0f;
        frameData.frameMs = static_cast<float>(GameData::deltaTime * 1000.0);
        frameData.playerPosition = m_runtime.player->getPosition();
        frameData.playerVelocity = m_runtime.player->getVelocity();
        frameData.flyMode = m_runtime.player->flyMode;
        frameData.onGround = m_runtime.player->isGrounded();
        frameData.renderDistance = m_runtime.player->renderDistance;
        frameData.remotePlayerCount = m_runtime.player->connectedPlayers.size();
        frameData.netConnected = m_runtime.clientNet.IsConnected();
        frameData.netStatus = m_runtime.clientNet.GetConnectionStatusText();
        frameData.serverTick = m_runtime.lastAppliedServerTick;
        frameData.ackedInputTick = m_runtime.lastAckedInputTick;
        frameData.pendingInputCount = m_runtime.pendingInputs.size();
        frameData.chunkDataQueueDepth = queueDepths.chunkData;
        frameData.chunkDeltaQueueDepth = queueDepths.chunkDelta;
        frameData.chunkUnloadQueueDepth = queueDepths.chunkUnload;
        frameData.backendName = simulation.renderCaps.backendName;
        frameData.mdiUsable = simulation.renderCaps.mdiUsable;
        frameData.perfFrameCpuMs = m_runtime.perf.frameCpuMs;
        frameData.perfInputMs = m_runtime.perf.inputMs;
        frameData.perfNetworkMs = m_runtime.perf.networkMs;
        frameData.perfPredictionMs = m_runtime.perf.predictionMs;
        frameData.perfGameplayMs = m_runtime.perf.gameplayMs;
        frameData.perfRenderCpuMs = m_runtime.perf.renderCpuMs;
        frameData.perfPresentMs = m_runtime.perf.presentMs;
        frameData.perfChunkStreamingMs = m_runtime.perf.chunkStreamingMs;
        m_runtime.renderer->appendBackendDebugUiFrameData(frameData);

        UiMutableState mutableState;
        mutableState.useDebugCamera = &m_app.m_UseDebugCamera;
        mutableState.toggleWireframe = &m_app.m_ToggleWireframe;
        mutableState.toggleChunkBorders = &m_app.m_ToggleChunkBorders;
        mutableState.toggleDebugFrustum = &m_app.m_ToggleDebugFrustum;
        mutableState.renderDistance = &m_runtime.player->renderDistance;
        mutableState.cursorEnabled = &GameData::cursorEnabled;
        mutableState.rawMouseInputEnabled = &m_app.m_EnableRawMouseInput;
        mutableState.rawMouseInputSupported = true;
        mutableState.gunViewOffset = &m_runtime.combat.equippedGunViewOffset;
        mutableState.gunViewScale = &m_runtime.combat.equippedGunViewScale;
        mutableState.gunViewEulerDeg = &m_runtime.combat.equippedGunViewEulerDeg;
        mutableState.sunDirection = &m_app.m_SunDirection;
        mutableState.sunShadowDirectionalBias = &m_app.m_SunShadowDirectionalBias;
        mutableState.sunShadowLowSunBiasBoost = &m_app.m_SunShadowLowSunBiasBoost;
        mutableState.sunShadowFrontFaceCullAtLowSun = &m_app.m_SunShadowFrontFaceCullAtLowSun;
        mutableState.sunShadowFrontFaceCullGrazingThreshold =
            &m_app.m_SunShadowFrontFaceCullGrazingThreshold;
        mutableState.skyExposure = &m_app.m_SkyExposure;
        mutableState.giTracingBackendPreference = simulation.renderCaps.supportsGiRuntimeControls
                                                      ? &GameData::giTracingBackendPreference
                                                      : nullptr;
        mutableState.giNrdDebugView =
            simulation.renderCaps.supportsGiRuntimeControls ? &GameData::giNrdDebugView : nullptr;

        m_runtime.debugUi->drawMainWindow(frameData, mutableState);
    }

    if (!m_runtime.debugUi->isVisible() && m_app.m_ShowDebugUi) {
        m_app.m_ShowDebugUi = false;
    }
    if (m_runtime.inventoryUi) {
        m_runtime.inventoryUi->draw(m_runtime.clientNet, m_runtime.clientNet.IsConnected());
        if (!m_runtime.inventoryUi->isVisible() && m_app.m_ShowInventoryUi) {
            m_app.m_ShowInventoryUi = false;
        }
    }

    GameData::cursorEnabled = m_app.m_ForceCursorEnabled || m_app.m_ShowDebugUi ||
                              m_app.m_ShowInventoryUi || !m_runtime.clientNet.IsConnected();
    m_app.applyMouseInputModes();

    ClientHudSystemContext hudCtx{};
    hudCtx.window = m_app.m_Window;
    hudCtx.serverIp = &m_app.m_ServerIp;
    hudCtx.serverPort = &m_app.m_ServerPort;
    hudCtx.requestedUsername = &m_app.m_RequestedUsername;
    hudCtx.beginConnectionAttempt = [this](Runtime &runtime) {
        return m_app.beginConnectionAttempt(runtime);
    };
    hudCtx.applyMouseInputModes = [this]() { m_app.applyMouseInputModes(); };
    m_hudSystem.draw(m_runtime, hudCtx);
    result.drawData = m_runtime.debugUi->endFrame();
    return result;
}

void FrameOrchestrator::runRenderStage(const SimulationStageResult &simulation,
                                       const UiStageResult &ui) {
    const Player::SimulationState simStateAfterPrediction =
        m_runtime.player->captureSimulationState();
    const glm::vec3 renderStateError =
        simStateAfterPrediction.position - m_runtime.renderCurrSimState.position;
    const float renderStateErrorSq = glm::dot(renderStateError, renderStateError);
    const float renderLatencyBlend = AppHelpers::LatencyCorrectionBlend(m_runtime.clientNet);
    const float renderSnapDist =
        Runtime::BasicAuthReconcileTeleportDistance + 5.5f + (4.0f * renderLatencyBlend);
    const float renderStateSnapDistSq = renderSnapDist * renderSnapDist;
    if (renderStateErrorSq > renderStateSnapDistSq) {
        m_runtime.renderPrevSimState = simStateAfterPrediction;
        m_runtime.renderCurrSimState = simStateAfterPrediction;
        m_runtime.hasSmoothedPlayerCameraPos = false;
    }

    const Camera &latestCamera = m_runtime.player->getCamera();
    m_runtime.interpolatedPlayerCamera = latestCamera;

    const float simAlpha = std::clamp(
        static_cast<float>(m_runtime.localSimAccumulator / Runtime::LocalPredictionStep), 0.0f, 1.0f);
    const glm::vec3 interpolatedBodyPos =
        glm::mix(m_runtime.renderPrevSimState.position, m_runtime.renderCurrSimState.position, simAlpha);
    const glm::vec3 extrapolatedBodyPos = m_runtime.renderCurrSimState.position +
                                          m_runtime.renderCurrSimState.velocity *
                                              static_cast<float>(m_runtime.localSimAccumulator);
    // Keep the local camera on the same timeline as local prediction.
    // Extra render extrapolation tends to overshoot during rapid strafe-turns
    // and shows up as visible camera jitter.
    float renderExtrapolationBlend = 0.0f;
    glm::vec3 targetBodyPos = glm::mix(interpolatedBodyPos, extrapolatedBodyPos, renderExtrapolationBlend);
    const glm::vec3 renderLead = targetBodyPos - m_runtime.renderCurrSimState.position;
    const float renderLeadLenSq = glm::dot(renderLead, renderLead);
    const float renderLeadMaxSq = Runtime::RenderLeadMaxDistance * Runtime::RenderLeadMaxDistance;
    if (renderLeadLenSq > renderLeadMaxSq && renderLeadLenSq > 1e-8f) {
        const float renderLeadLen = std::sqrt(renderLeadLenSq);
        targetBodyPos = m_runtime.renderCurrSimState.position +
                        renderLead * (Runtime::RenderLeadMaxDistance / renderLeadLen);
    }
    const float interpolatedStepOffset =
        glm::mix(m_runtime.renderPrevSimState.stepUpVisualOffset,
                 m_runtime.renderCurrSimState.stepUpVisualOffset, simAlpha);
    const float eyeHeight = Shared::PlayerData::GetMovementSettings().eyeHeight;
    const glm::vec3 targetCameraPos =
        targetBodyPos + glm::vec3(0.0f, eyeHeight - interpolatedStepOffset, 0.0f);
    m_runtime.smoothedPlayerCameraPos = targetCameraPos;
    m_runtime.hasSmoothedPlayerCameraPos = true;
    m_runtime.interpolatedPlayerCamera.position = targetCameraPos;

    const float worldItemBlend =
        std::clamp(1.0f - std::exp(-14.0f * static_cast<float>(GameData::deltaTime)), 0.0f, 1.0f);
    for (auto &[_, item] : m_runtime.worldItems) {
        item.position = glm::mix(item.position, item.targetPosition, worldItemBlend);
    }

    const Camera &activeCamera =
        m_app.m_UseDebugCamera ? m_runtime.debugCamera : m_runtime.interpolatedPlayerCamera;
    const Camera &cullingCamera = m_runtime.interpolatedPlayerCamera;

    const float sunDirLenSq = glm::dot(m_app.m_SunDirection, m_app.m_SunDirection);
    if (!std::isfinite(sunDirLenSq) || sunDirLenSq <= 1e-8f) {
        m_app.m_SunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    RenderFrameParams frameParams{
        .debugShader = m_runtime.dbgShader.get(),
        .chunkManager = *m_runtime.chunkManager,
        .frustum = m_runtime.frustum,
        .player = *m_runtime.player,
        .activeCamera = activeCamera,
        .cullingCamera = &cullingCamera,
        .sunDirection = m_app.m_SunDirection,
        .skyExposure = m_app.m_SkyExposure,
        .toggleWireframe = m_app.m_ToggleWireframe,
        .toggleChunkBorders = m_app.m_ToggleChunkBorders,
        .toggleDebugFrustum = m_app.m_ToggleDebugFrustum,
        .sunShadowDirectionalBias = m_app.m_SunShadowDirectionalBias,
        .sunShadowLowSunBiasBoost = m_app.m_SunShadowLowSunBiasBoost,
        .sunShadowFrontFaceCullAtLowSun = m_app.m_SunShadowFrontFaceCullAtLowSun,
        .sunShadowFrontFaceCullGrazingThreshold = m_app.m_SunShadowFrontFaceCullGrazingThreshold,
        .uiDrawData = ui.drawData,
        .renderOpaqueOverlayPasses = [&]() {
            m_app.renderWorldItems(m_runtime, activeCamera);
            // Keep remote gun overlays out of first-person scene capture to avoid
            // self-echo/near-camera artifacts that can mask terrain.
            if (m_app.m_UseDebugCamera && m_runtime.gunSceneRenderer) {
                m_runtime.gunSceneRenderer->renderRemotePlayerGuns(m_runtime, activeCamera,
                                                                   m_app.m_SunDirection);
            }
        }};
    m_runtime.renderer->renderFrame(frameParams);

    if (simulation.renderCaps.supportsFirstPersonViewmodel && !m_app.m_UseDebugCamera &&
        m_runtime.combat.localPlayerAlive && m_runtime.gunSceneRenderer) {
        m_runtime.gunSceneRenderer->renderHeldGun(m_runtime, m_runtime.interpolatedPlayerCamera,
                                                  m_app.m_SunDirection);
    }
    if (m_runtime.debugUi && !simulation.renderCaps.compositesUiInRenderFrame) {
        m_runtime.debugUi->renderDrawData(ui.drawData);
    }
}

void FrameOrchestrator::updateFrameHotkeysAndCounters() {
    static bool f11PressedLastFrame = false;
    const bool f11PressedNow = IsScancodeDown(SDL_SCANCODE_F11);
    if (f11PressedNow && !f11PressedLastFrame) {
        m_app.toggleFullscreen(m_app.m_Window);
    }
    f11PressedLastFrame = f11PressedNow;
    m_app.updateFPSCounter();
}

void FrameOrchestrator::runPresentStage(size_t localPredictionSteps) {
    const auto perfPresentStart = Clock::now();
    m_runtime.renderer->present(m_app.m_Window);
    m_app.pollEvents(m_runtime);
    const auto perfPresentEnd = Clock::now();
    m_runtime.perf.presentMs = MeasureMs(perfPresentStart, perfPresentEnd);

    const ClientNetwork::ChunkQueueDepths queueDepths = m_runtime.clientNet.GetChunkQueueDepths();
    const bool frameUnderPressure = GameData::deltaTime > (Runtime::LocalPredictionStep * 1.2);
    const bool chunkBacklog = queueDepths.chunkData > (Runtime::MaxChunkDataApplyPerFrame * 3) ||
                              queueDepths.chunkDelta > (Runtime::MaxChunkDeltaApplyPerFrame * 3) ||
                              queueDepths.chunkUnload > (Runtime::MaxChunkUnloadApplyPerFrame * 3);
    const bool prioritizeMovement = (localPredictionSteps > 1) || frameUnderPressure || chunkBacklog;
    const auto perfChunkStart = Clock::now();
    m_app.processChunkStreaming(m_runtime, prioritizeMovement);
    const auto perfChunkEnd = Clock::now();
    m_runtime.perf.chunkStreamingMs = MeasureMs(perfChunkStart, perfChunkEnd);
}
