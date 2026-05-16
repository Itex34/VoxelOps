#include "RenderSceneBuilder.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"
#include "../runtime/Runtime.hpp"

#include "../../Shared/player/PlayerData.hpp"

#include <algorithm>
#include <cmath>

RenderScene RenderSceneBuilder::build(Runtime &runtime, const RenderSceneBuilderInput &input) const {
    const Player::SimulationState simStateAfterPrediction =
        runtime.gameplay.player->captureSimulationState();
    const glm::vec3 renderStateError =
        simStateAfterPrediction.position - runtime.prediction.renderCurrSimState.position;
    const float renderStateErrorSq = glm::dot(renderStateError, renderStateError);
    const float renderLatencyBlend = AppHelpers::LatencyCorrectionBlend(runtime.network.clientNet);
    const float renderSnapDist =
        RuntimePredictionState::BasicAuthReconcileTeleportDistance + 5.5f + (4.0f * renderLatencyBlend);
    const float renderStateSnapDistSq = renderSnapDist * renderSnapDist;
    if (renderStateErrorSq > renderStateSnapDistSq) {
        runtime.prediction.renderPrevSimState = simStateAfterPrediction;
        runtime.prediction.renderCurrSimState = simStateAfterPrediction;
        runtime.prediction.hasSmoothedPlayerCameraPos = false;
    }

    const Camera &latestCamera = runtime.gameplay.player->getCamera();
    runtime.render.interpolatedPlayerCamera = latestCamera;

    const float simAlpha = std::clamp(
        static_cast<float>(
            runtime.prediction.localSimAccumulator / RuntimePredictionState::LocalPredictionStep
        ),
        0.0f,
        1.0f
    );
    const glm::vec3 interpolatedBodyPos = glm::mix(
        runtime.prediction.renderPrevSimState.position,
        runtime.prediction.renderCurrSimState.position,
        simAlpha
    );
    const glm::vec3 extrapolatedBodyPos =
        runtime.prediction.renderCurrSimState.position +
        runtime.prediction.renderCurrSimState.velocity *
            static_cast<float>(runtime.prediction.localSimAccumulator);
    const float renderExtrapolationBlend = 0.0f;
    glm::vec3 targetBodyPos =
        glm::mix(interpolatedBodyPos, extrapolatedBodyPos, renderExtrapolationBlend);
    const glm::vec3 renderLead = targetBodyPos - runtime.prediction.renderCurrSimState.position;
    const float renderLeadLenSq = glm::dot(renderLead, renderLead);
    const float renderLeadMaxSq = RuntimePredictionState::RenderLeadMaxDistance *
                                  RuntimePredictionState::RenderLeadMaxDistance;
    if (renderLeadLenSq > renderLeadMaxSq && renderLeadLenSq > 1e-8f) {
        const float renderLeadLen = std::sqrt(renderLeadLenSq);
        targetBodyPos = runtime.prediction.renderCurrSimState.position +
                        renderLead *
                            (RuntimePredictionState::RenderLeadMaxDistance / renderLeadLen);
    }
    const float interpolatedStepOffset = glm::mix(
        runtime.prediction.renderPrevPresentationState.stepUpVisualOffset,
        runtime.prediction.renderCurrPresentationState.stepUpVisualOffset,
        simAlpha
    );
    const float eyeHeight = Shared::PlayerData::GetMovementSettings().eyeHeight;
    const glm::vec3 targetCameraPos =
        targetBodyPos + glm::vec3(0.0f, eyeHeight - interpolatedStepOffset, 0.0f);
    runtime.prediction.smoothedPlayerCameraPos = targetCameraPos;
    runtime.prediction.hasSmoothedPlayerCameraPos = true;
    runtime.render.interpolatedPlayerCamera.position = targetCameraPos;

    const float worldItemBlend =
        std::clamp(1.0f - std::exp(-14.0f * static_cast<float>(GameData::deltaTime)), 0.0f, 1.0f);
    for (auto &[_, item] : runtime.world.worldItems) {
        item.position = glm::mix(item.position, item.targetPosition, worldItemBlend);
    }

    const Camera &activeCamera = input.useDebugCamera ? runtime.render.debugCamera
                                                      : runtime.render.interpolatedPlayerCamera;
    const Camera &cullingCamera = runtime.render.interpolatedPlayerCamera;

    glm::vec3 sunDirection = input.sunDirection;
    const float sunDirLenSq = glm::dot(sunDirection, sunDirection);
    if (!std::isfinite(sunDirLenSq) || sunDirLenSq <= 1e-8f) {
        sunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    return RenderScene{
        .chunkWorld =
            RenderChunkWorldView{
                .cpuChunkMeshes = &runtime.gameplay.chunkManager->getCpuChunkMeshes(),
                .cpuChunkMeshesVersion = runtime.gameplay.chunkManager->getCpuChunkMeshesVersion(),
                .chunks = &runtime.gameplay.chunkManager->getChunks(),
                .enableAO = runtime.gameplay.chunkManager->enableAO
            },
        .activeCamera = activeCamera,
        .cullingCamera = &cullingCamera,
        .localPlayerPosition = runtime.gameplay.player->getPosition(),
        .chunkRenderDistance = runtime.gameplay.player->renderDistance,
        .remotePlayers = [&]() {
            std::vector<RenderRemotePlayerState> out;
            out.reserve(runtime.gameplay.player->connectedPlayers.size());
            for (const auto &[_, state] : runtime.gameplay.player->connectedPlayers) {
                out.push_back(
                    RenderRemotePlayerState{
                        .position = state.position,
                        .rotation = state.rotation,
                        .scale = state.scale,
                        .weaponId = state.weaponId
                    }
                );
            }
            return out;
        }(),
        .sunDirection = sunDirection,
        .skyExposure = input.skyExposure,
        .toggleWireframe = input.toggleWireframe,
        .toggleChunkBorders = input.toggleChunkBorders,
        .toggleDebugFrustum = input.toggleDebugFrustum,
        .sunShadowDirectionalBias = input.sunShadowDirectionalBias,
        .sunShadowLowSunBiasBoost = input.sunShadowLowSunBiasBoost,
        .sunShadowFrontFaceCullAtLowSun = input.sunShadowFrontFaceCullAtLowSun,
        .sunShadowFrontFaceCullGrazingThreshold = input.sunShadowFrontFaceCullGrazingThreshold,
        .uiDrawData = input.uiDrawData,
        .renderOpaqueOverlayPasses = input.renderOpaqueOverlayPasses,
        .useDebugCamera = input.useDebugCamera
    };
}
