#include "VulkanFrameBuilder.hpp"

#include "../Camera.hpp"
#include "../Frustum.hpp"
#include "../../data/GameData.hpp"
#include "../../voxels/Chunk.hpp"
#include "../../../Shared/player/PlayerData.hpp"
#include "graphics/Model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

VulkanFrameBuildResult VulkanFrameBuilder::buildFrameData(
    FrameRenderData &frameData,
    const VulkanChunkRenderCache &chunkRenderCache,
    const VkTexture &atlasTexture,
    const Camera &activeCamera,
    const Camera &cullingCamera,
    const glm::vec3 &localPlayerPosition,
    uint16_t chunkRenderDistance,
    const std::vector<RenderRemotePlayerState> &remotePlayers,
    ImDrawData *uiDrawData,
    int width,
    int height,
    const VkModel *remotePlayerModel,
    const std::vector<const VkTexture *> &remotePlayerTextures,
    const glm::ivec3 &cullingChunk
) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };

    VulkanFrameBuildResult out{};
    out.view = activeCamera.getViewMatrix();
    out.projection = glm::perspectiveRH_ZO(
        glm::radians(GameData::FOV),
        static_cast<float>(width) / static_cast<float>(height),
        0.1f,
        1000.0f
    );
    out.projection[1][1] *= -1.0f;
    out.viewProjection = out.projection * out.view;

    Frustum frustum;
    const glm::mat4 cullingViewProjection = out.projection * cullingCamera.getViewMatrix();
    frustum.extractPlanes(cullingViewProjection, true);

    const int maxRenderDistance = std::max(2, static_cast<int>(chunkRenderDistance));
    const int64_t radius2 =
        static_cast<int64_t>(maxRenderDistance) * static_cast<int64_t>(maxRenderDistance);
    const auto frameBuildStart = std::chrono::steady_clock::now();

    frameData.modelMatrices.reserve(chunkRenderCache.size());
    frameData.indirectCommands.reserve(chunkRenderCache.size());
    frameData.indirectBatches.reserve(chunkRenderCache.size());
    frameData.uiDrawData = uiDrawData;

    for (const auto &[chunkPos, cached] : chunkRenderCache.getChunkMeshes()) {
        if (cached.mesh.getIndexCount() == 0) {
            continue;
        }

        const glm::ivec3 d = chunkPos - cullingChunk;
        const int64_t dist2 = static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
                              static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
        if (dist2 > radius2) {
            continue;
        }

        const glm::vec3 chunkMin = glm::vec3(chunkPos * CHUNK_SIZE);
        const glm::vec3 chunkMax = chunkMin + glm::vec3(CHUNK_SIZE);
        if (!frustum.isBoxVisible(chunkMin, chunkMax)) {
            continue;
        }

        const glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMin);
        const uint32_t modelIndex = static_cast<uint32_t>(frameData.modelMatrices.size());
        frameData.modelMatrices.push_back(model);

        const uint32_t commandIndex = static_cast<uint32_t>(frameData.indirectCommands.size());
        IndexedIndirectCommand command{};
        command.indexCount = cached.mesh.getIndexCount();
        command.instanceCount = 1;
        command.firstIndex = 0;
        command.vertexOffset = 0;
        command.firstInstance = modelIndex;
        frameData.indirectCommands.push_back(command);

        RenderIndirectBatch batch{};
        batch.mesh = &cached.mesh;
        batch.texture = &atlasTexture;
        batch.firstCommand = commandIndex;
        batch.commandCount = 1;
        frameData.indirectBatches.push_back(batch);
    }

    if (remotePlayerModel && remotePlayerModel->hasLocalBounds()) {
        constexpr float kLocalGhostRejectDistance = 2.0f;
        const float localGhostRejectDistanceSq =
            kLocalGhostRejectDistance * kLocalGhostRejectDistance;

        const glm::vec3 localMin = remotePlayerModel->getLocalMinBounds();
        const glm::vec3 localMax = remotePlayerModel->getLocalMaxBounds();
        const glm::vec3 modelSize = localMax - localMin;
        const float targetHeight =
            std::max(Shared::PlayerData::GetMovementSettings().collisionHeight, 0.01f);
        const float uniformFitToCollision = targetHeight / std::max(modelSize.y, 1.0e-4f);
        const float modelMinY = localMin.y;

        frameData.objects.reserve(frameData.objects.size() + remotePlayers.size());
        for (const RenderRemotePlayerState &state : remotePlayers) {
            const glm::vec3 toLocal = state.position - localPlayerPosition;
            const float localDistSq = glm::dot(toLocal, toLocal);
            if (!std::isfinite(localDistSq) || localDistSq < localGhostRejectDistanceSq) {
                continue;
            }

            const glm::vec3 scaled = state.scale * uniformFitToCollision;
            const glm::vec3 anchoredPos =
                state.position + glm::vec3(0.0f, -modelMinY * scaled.y, 0.0f);
            const glm::quat safeRotation = (glm::dot(state.rotation, state.rotation) > 1.0e-10f)
                                               ? glm::normalize(state.rotation)
                                               : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

            RenderObject object{};
            object.model = remotePlayerModel;
            object.meshTextures = &remotePlayerTextures;
            object.transform = glm::translate(glm::mat4(1.0f), anchoredPos) *
                               glm::toMat4(safeRotation) * glm::scale(glm::mat4(1.0f), scaled);
            frameData.objects.push_back(object);
        }
    }

    const auto frameBuildEnd = std::chrono::steady_clock::now();
    out.cpuFrameBuildMs = measureMs(frameBuildStart, frameBuildEnd);
    return out;
}
