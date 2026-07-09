#include "FrameOrchestrator.hpp"

#include "AppHelpers.hpp"
#include "../graphics/IGunSceneRenderer.hpp"
#include "../graphics/Camera.hpp"
#include "../render/RenderScene.hpp"
#include "../data/GameData.hpp"
#include "../../Shared/network/Packets.hpp"
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <utility>

#include <glm/geometric.hpp>

namespace {
    using Clock = std::chrono::steady_clock;
    constexpr double kBotPredictionStep = 1.0 / 30.0;
    constexpr uint32_t kBotMaxUnackedShootLeadTicks = 12;

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

    bool HasMoveIntent(const NetworkInputState &input) {
        constexpr uint8_t kMoveFlags = kPlayerInputFlagForward | kPlayerInputFlagBackward |
                                       kPlayerInputFlagLeft | kPlayerInputFlagRight |
                                       kPlayerInputFlagFlyUp | kPlayerInputFlagFlyDown;
        constexpr float kMoveAxisEps = 0.001f;
        return (input.flags & kMoveFlags) != 0 || std::abs(input.moveX) > kMoveAxisEps ||
               std::abs(input.moveZ) > kMoveAxisEps;
    }

    float RandomFloat(std::mt19937 &rng, float minValue, float maxValue) {
        std::uniform_real_distribution<float> dist(minValue, maxValue);
        return dist(rng);
    }

    bool RandomChance(std::mt19937 &rng, float probability) {
        return RandomFloat(rng, 0.0f, 1.0f) < probability;
    }

    glm::vec3
    ComputeViewmodelMuzzlePosition(const Camera &camera, const RuntimeCombatState &combat) {
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

bool FrameOrchestrator::isBotModeEnabled() const {
    return m_context.simulation.botMode != nullptr && *m_context.simulation.botMode;
}

ClientInputIntent FrameOrchestrator::buildBotInputIntent() {
    ClientInputIntent intent{};
    Runtime &rt = runtime();
    if (!rt.gameplay.player) {
        return intent;
    }

    if (!m_botInitialized) {
        uint32_t seed = 0;
        if (m_context.simulation.botSeed != nullptr && *m_context.simulation.botSeed != 0) {
            seed = *m_context.simulation.botSeed;
        } else {
            seed = static_cast<uint32_t>(
                Clock::now().time_since_epoch().count() ^
                static_cast<int64_t>(reinterpret_cast<uintptr_t>(this))
            );
        }
        m_botRng.seed(seed);
        m_botYaw = RandomFloat(m_botRng, -180.0f, 180.0f);
        m_botPitch = RandomFloat(m_botRng, -8.0f, 8.0f);
        m_botNextDecisionTime = 0.0;
        m_botNextShotTime = m_frameNow + RandomFloat(m_botRng, 0.25f, 1.0f);
        m_botInitialized = true;
    }

    if (m_frameNow >= m_botNextDecisionTime) {
        const float localForward = RandomFloat(m_botRng, -0.25f, 1.0f);
        const float localStrafe = RandomFloat(m_botRng, -1.0f, 1.0f);
        glm::vec2 localMove(localStrafe, localForward);
        if (glm::length(localMove) > 1.0f) {
            localMove = glm::normalize(localMove);
        }

        m_botYaw = AppHelpers::NormalizeYawDegrees(m_botYaw + RandomFloat(m_botRng, -80.0f, 80.0f));
        m_botPitch = std::clamp(m_botPitch + RandomFloat(m_botRng, -8.0f, 8.0f), -30.0f, 30.0f);

        const float yawRad = glm::radians(m_botYaw);
        const glm::vec2 forward2D(std::cos(yawRad), std::sin(yawRad));
        const glm::vec2 right2D(-forward2D.y, forward2D.x);
        const glm::vec2 worldMove = right2D * localMove.x + forward2D * localMove.y;

        m_botInput.moveX = worldMove.x;
        m_botInput.moveZ = worldMove.y;
        m_botInput.yaw = m_botYaw;
        m_botInput.pitch = m_botPitch;
        m_botInput.flyMode = false;
        m_botInput.flags = 0;
        if (localMove.y > 0.15f) {
            m_botInput.flags |= kPlayerInputFlagForward;
        }
        if (localMove.y < -0.15f) {
            m_botInput.flags |= kPlayerInputFlagBackward;
        }
        if (localMove.x < -0.15f) {
            m_botInput.flags |= kPlayerInputFlagLeft;
        }
        if (localMove.x > 0.15f) {
            m_botInput.flags |= kPlayerInputFlagRight;
        }
        if (RandomChance(m_botRng, 0.75f)) {
            m_botInput.flags |= kPlayerInputFlagSprint;
        }
        if (RandomChance(m_botRng, 0.18f)) {
            m_botInput.flags |= kPlayerInputFlagJump;
        }

        m_botNextDecisionTime = m_frameNow + RandomFloat(m_botRng, 0.20f, 0.90f);
    } else {
        m_botInput.yaw = AppHelpers::NormalizeYawDegrees(
            m_botInput.yaw +
            RandomFloat(m_botRng, -28.0f, 28.0f) * static_cast<float>(GameData::deltaTime)
        );
        m_botInput.pitch = std::clamp(
            m_botInput.pitch +
                RandomFloat(m_botRng, -8.0f, 8.0f) * static_cast<float>(GameData::deltaTime),
            -30.0f,
            30.0f
        );
        m_botYaw = m_botInput.yaw;
        m_botPitch = m_botInput.pitch;
    }

    intent.gameplayInputEnabled = true;
    intent.networkInput = m_botInput;
    rt.gameplay.player->setNetworkInputState(intent.networkInput);
    return intent;
}

void FrameOrchestrator::maybeSendBotShot() {
    Runtime &rt = runtime();
    if (!rt.combat.localPlayerAlive || !rt.network.clientNet.IsConnected() ||
        !rt.combat.equippedGun || !rt.gameplay.player) {
        return;
    }

    float shootRate = 1.5f;
    if (m_context.simulation.botShootRate != nullptr) {
        shootRate = std::max(0.05f, *m_context.simulation.botShootRate);
    }

    if (m_frameNow < m_botNextShotTime) {
        return;
    }
    if ((m_frameNow - rt.combat.lastShootSendTime) < rt.combat.shootSendInterval) {
        return;
    }
    if (!rt.prediction.hasAppliedServerTick || rt.prediction.lastAckedInputTick == 0 ||
        rt.prediction.inputTickCounter == 0) {
        return;
    }

    const uint32_t latestSentInputTick = rt.prediction.inputTickCounter - 1u;
    if (latestSentInputTick < rt.prediction.lastAckedInputTick ||
        static_cast<uint32_t>(latestSentInputTick - rt.prediction.lastAckedInputTick) >
            kBotMaxUnackedShootLeadTicks) {
        return;
    }

    const Camera &cam = rt.gameplay.player->getCamera();
    const float dirLenSq = glm::dot(cam.front, cam.front);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        return;
    }

    glm::vec3 shootDir = glm::normalize(cam.front);
    shootDir.x += RandomFloat(m_botRng, -0.035f, 0.035f);
    shootDir.y += RandomFloat(m_botRng, -0.020f, 0.020f);
    shootDir.z += RandomFloat(m_botRng, -0.035f, 0.035f);
    const float jitteredLenSq = glm::dot(shootDir, shootDir);
    if (std::isfinite(jitteredLenSq) && jitteredLenSq >= 1e-8f) {
        shootDir = glm::normalize(shootDir);
    } else {
        shootDir = glm::normalize(cam.front);
    }

    const uint32_t shotId = rt.combat.nextClientShotId++;
    const uint32_t clientTick = rt.prediction.lastAckedInputTick;
    const uint32_t seed = shotId ^ (clientTick * 2654435761u);
    if (rt.network.clientNet.SendShootRequest(
            shotId,
            clientTick,
            rt.combat.equippedGun->getWeaponId(),
            cam.position,
            shootDir,
            seed,
            m_botInput.flags
        )) {
        rt.combat.lastShootSendTime = m_frameNow;
        rt.combat.hasLastLocalShot = true;
        rt.combat.lastLocalShotId = shotId;
        rt.combat.lastLocalShotOrigin = cam.position;
        rt.combat.lastLocalShotDirection = shootDir;
        rt.combat.lastLocalShotTime = m_frameNow;
    }

    const double meanInterval = 1.0 / static_cast<double>(std::max(0.05f, shootRate));
    m_botNextShotTime = m_frameNow + RandomFloat(
                                         m_botRng,
                                         static_cast<float>(meanInterval * 0.55),
                                         static_cast<float>(meanInterval * 1.65)
                                     );
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
        runtime().network.clientNet.IsConnected() && (runtime().ui.activeView == UiView::InGame) &&
        (m_context.ui.showDebugUi != nullptr) && (m_context.ui.showInventoryUi != nullptr) &&
        (m_context.simulation.forceCursorEnabled != nullptr) && !*m_context.ui.showDebugUi &&
        !*m_context.ui.showInventoryUi && !runtime().ui.pauseMenuVisible &&
        !*m_context.simulation.forceCursorEnabled;

    if (!isBotModeEnabled()) {
        runtime().gameplay.inputCallbacks->processInput(m_context.host.window);
    }
    if (windowHost != nullptr && !isBotModeEnabled()) {
        windowHost->applyMouseInputModes();
    }
    const ClientInputIntent inputIntent =
        isBotModeEnabled() ? buildBotInputIntent()
                           : m_inputSystem.captureIntent(runtime(), m_context.host.window);
    const bool botMode = isBotModeEnabled();
    const double predictionStep =
        botMode ? kBotPredictionStep : RuntimePredictionState::LocalPredictionStep;
    runtime().prediction.localSimAccumulator += GameData::deltaTime;
    const auto perfInputEnd = Clock::now();
    runtime().app.perf.inputMs = MeasureMs(perfInputStart, perfInputEnd);

    const double maxAccumulatedTime =
        predictionStep *
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
        const Player::SimulationState initialSimState =
            runtime().gameplay.player->captureSimulationState();
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
        while (runtime().prediction.localSimAccumulator >= predictionStep &&
               localPredictionSteps < RuntimePredictionState::MaxLocalPredictionStepsPerFrame) {
            (void)m_clientSession.sendPredictedInputTick(
                runtime(), inputIntent, predictionStep, !botMode
            );
            m_clientPrediction.update(runtime(), inputIntent, predictionStep);
            runtime().prediction.localSimAccumulator -= predictionStep;
            runtime().prediction.renderPrevSimState = runtime().prediction.renderCurrSimState;
            runtime().prediction.renderCurrSimState =
                runtime().gameplay.player->captureSimulationState();
            runtime().prediction.renderPrevPresentationState =
                runtime().prediction.renderCurrPresentationState;
            runtime().prediction.renderCurrPresentationState =
                runtime().gameplay.player->capturePresentationState();
            ++localPredictionSteps;
        }
        if (localPredictionSteps == 0) {
            // Keep player's network-input state synchronized even if no fixed-step sim ran.
            m_clientPrediction.update(runtime(), inputIntent, 0.0);
        }
        if (localPredictionSteps == RuntimePredictionState::MaxLocalPredictionStepsPerFrame &&
            runtime().prediction.localSimAccumulator >= predictionStep) {
            runtime().prediction.localSimAccumulator =
                std::fmod(runtime().prediction.localSimAccumulator, predictionStep);
        }
        if (!HasMoveIntent(inputIntent.networkInput)) {
            const Player::SimulationState stoppedState =
                runtime().gameplay.player->captureSimulationState();
            const glm::vec2 horizontalVelocity(stoppedState.velocity.x, stoppedState.velocity.z);
            constexpr float kStoppedVelocityEps = 0.01f;
            if (stoppedState.onGround && glm::dot(horizontalVelocity, horizontalVelocity) <=
                                             kStoppedVelocityEps * kStoppedVelocityEps) {
                const Player::PresentationState stoppedPresentationState =
                    runtime().gameplay.player->capturePresentationState();
                runtime().prediction.renderPrevSimState = stoppedState;
                runtime().prediction.renderCurrSimState = stoppedState;
                runtime().prediction.renderPrevPresentationState = stoppedPresentationState;
                runtime().prediction.renderCurrPresentationState = stoppedPresentationState;
            }
        }
    } else {
        runtime().prediction.localSimAccumulator = 0.0;
        runtime().world.renderStateNeedsResync = false;
        const Player::SimulationState frozenState =
            runtime().gameplay.player->captureSimulationState();
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
    if (isBotModeEnabled()) {
        maybeSendBotShot();
    } else {
        m_combatShootSystem.update(runtime(), combatShootCtx);
    }
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

    if (runtime().ui.nativeUi) {
        runtime().ui.nativeUi->beginFrame(static_cast<float>(GameData::deltaTime));
        GameData::uiWantsMouseCapture = runtime().ui.nativeUi->wantsMouseCapture();
        GameData::uiWantsKeyboardCapture = runtime().ui.nativeUi->wantsKeyboardCapture();
        GameData::uiWantsTextInput = runtime().ui.nativeUi->wantsKeyboardCapture();
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        const ImGuiIO &io = ImGui::GetIO();
        GameData::uiWantsMouseCapture = GameData::uiWantsMouseCapture || io.WantCaptureMouse;
        GameData::uiWantsKeyboardCapture =
            GameData::uiWantsKeyboardCapture || io.WantCaptureKeyboard;
        GameData::uiWantsTextInput = GameData::uiWantsTextInput || io.WantTextInput;
    }

    if (runtime().ui.debugUi) {
        const bool nativeOwnsHud =
            runtime().ui.nativeUi && runtime().ui.nativeUi->hasBackendRenderer();
        if (!nativeOwnsHud) {
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
        mutableState.sunShadowFrontFaceCullAtLowSun =
            m_context.render.sunShadowFrontFaceCullAtLowSun;
        mutableState.sunShadowFrontFaceCullGrazingThreshold =
            m_context.render.sunShadowFrontFaceCullGrazingThreshold;
        mutableState.skyExposure = m_context.render.skyExposure;
        mutableState.giTracingBackendPreference =
            simulation.renderCapabilities.supportsGiRuntimeControls
                ? &GameData::giTracingBackendPreference
                : nullptr;
        mutableState.giNrdDebugView = simulation.renderCapabilities.supportsGiRuntimeControls
                                          ? &GameData::giNrdDebugView
                                          : nullptr;
        mutableState.giNrdGuideOverride = simulation.renderCapabilities.supportsGiRuntimeControls
                                              ? &GameData::giNrdGuideOverride
                                              : nullptr;
        mutableState.isVulkanActive = simulation.renderCapabilities.api == RenderApi::Vulkan;
        mutableState.isOpenGlActive = simulation.renderCapabilities.api == RenderApi::OpenGL;

        mutableState.requestSwitchToOpenGl = m_context.ui.requestSwitchToOpenGl;
        mutableState.requestSwitchToVulkan = m_context.ui.requestSwitchToVulkan;
        mutableState.renderApiPreference = m_context.ui.renderApiPreference;

        runtime().ui.debugUi->drawMainWindow(frameData, mutableState);
    }

    if (runtime().ui.debugUi && !runtime().ui.debugUi->isVisible() &&
        m_context.ui.showDebugUi != nullptr && *m_context.ui.showDebugUi) {
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
    GameData::cursorEnabled = forceCursor || showDebugUi || showInventoryUi ||
                              runtime().ui.pauseMenuVisible || runtime().ui.wantsCursor();
    if (!GameData::cursorEnabled && runtime().ui.activeView == UiView::InGame) {
        GameData::uiWantsMouseCapture = false;
    }
    if (m_context.windowHost != nullptr) {
        m_context.windowHost->applyMouseInputModes();
    }

    m_hudSystem.draw(runtime());
    m_pauseMenu.draw(runtime(), m_context.connectionHost, m_context.windowHost);
    m_settingsMenu.draw(runtime(), m_context.connectionHost, m_context.windowHost);

    if (runtime().ui.nativeUi) {
        runtime().ui.nativeUi->endFrame();
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
    sceneInput.nativeUiDrawData =
        runtime().ui.nativeUi ? runtime().ui.nativeUi->drawData() : nullptr;

    RenderScene frameScene = m_renderSceneBuilder.build(runtime(), sceneInput);
    const bool renderDebugLine = simulation.renderCapabilities.api == RenderApi::OpenGL;
    const bool renderGrappleRope =
        renderDebugLine && runtime().combat.grapple.isAttached && runtime().combat.localPlayerAlive;
    constexpr double kAcceptedShotLineDurationSeconds = 0.30;
    constexpr double kLocalShotFallbackDurationSeconds = 0.10;
    const bool acceptedMatchesLocalShot =
        runtime().combat.hasLastAcceptedShotResult && runtime().combat.hasLastLocalShot &&
        (runtime().combat.lastAcceptedShotResultId == runtime().combat.lastLocalShotId);
    const bool drawAcceptedShotConfirmation =
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
        if (drawLocalShotFallback) {
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
        if (drawAcceptedShotConfirmation) {
            const glm::vec3 hitPoint = runtime().combat.lastAcceptedShotResultPoint;
            const glm::vec3 hitUp(0.0f, 0.10f, 0.0f);
            const glm::vec3 hitBack = runtime().combat.lastLocalShotDirection * -0.25f;
            m_debugRenderer.drawLine(
                hitPoint - hitUp,
                hitPoint + hitUp,
                glm::vec3(1.0f, 0.9f, 0.2f),
                LineDrawMode::TwoVertices,
                1.0f,
                frameScene.activeCamera
            );
            m_debugRenderer.drawLine(
                hitPoint,
                hitPoint + hitBack,
                glm::vec3(1.0f, 0.9f, 0.2f),
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

    const ClientNetwork::ChunkQueueDepths queueDepths =
        runtime().network.clientNet.GetChunkQueueDepths();
    constexpr double kSmoothStreamingFrameBudgetSec = 1.0 / 90.0;
    const bool frameUnderPressure = GameData::deltaTime > kSmoothStreamingFrameBudgetSec;
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
