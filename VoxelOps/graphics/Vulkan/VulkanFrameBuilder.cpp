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
#include <cstdlib>
#include <future>
#include <iterator>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace {
    bool parseBoolEnv(const char *name, bool defaultValue) {
        const char *env = std::getenv(name);
        if (env == nullptr) {
            return defaultValue;
        }
        if (env[0] == '1' || env[0] == 't' || env[0] == 'T' || env[0] == 'y' || env[0] == 'Y') {
            return true;
        }
        if (env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N') {
            return false;
        }
        return defaultValue;
    }

    bool chunkSuperbatchEnabled() {
        static const bool enabled = parseBoolEnv("VOXELOPS_VK_CHUNK_SUPERBATCH", true);
        return enabled;
    }
} // namespace

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
    const NativeUiDrawData *nativeUiDrawData,
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

    frameData.uiDrawData = uiDrawData;
    frameData.nativeUiDrawData = nativeUiDrawData;

    struct VisibleChunkPacket {
        const VkMesh *mesh = nullptr;
        glm::mat4 model{1.0f};
    };

    const auto &chunkMeshes = chunkRenderCache.getChunkMeshes();
    std::vector<const std::pair<const glm::ivec3, VulkanChunkRenderCache::CachedChunkMesh> *> entries;
    entries.reserve(chunkMeshes.size());
    for (const auto &entry : chunkMeshes) {
        entries.push_back(&entry);
    }

    auto buildVisibleChunkPackets = [
        &entries, &frustum, &cullingChunk, radius2
    ](size_t beginIndex, size_t endIndex) {
        std::vector<VisibleChunkPacket> localPackets;
        localPackets.reserve(endIndex - beginIndex);

        for (size_t i = beginIndex; i < endIndex; ++i) {
            const auto &entry = *entries[i];
            const glm::ivec3 &chunkPos = entry.first;
            const VulkanChunkRenderCache::CachedChunkMesh &cached = entry.second;
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

            VisibleChunkPacket packet{};
            packet.mesh = &cached.mesh;
            packet.model = glm::translate(glm::mat4(1.0f), chunkMin);
            localPackets.push_back(packet);
        }

        return localPackets;
    };

    std::vector<VisibleChunkPacket> visibleChunkPackets;
    if (!entries.empty()) {
        const unsigned hwThreads = std::max(1u, std::thread::hardware_concurrency());
        const size_t maxWorkers = std::min<size_t>(static_cast<size_t>(hwThreads), entries.size());
        constexpr size_t kMinEntriesPerWorker = 128u;
        const size_t workerCount =
            std::max<size_t>(1u, std::min(maxWorkers, entries.size() / kMinEntriesPerWorker));

        if (workerCount <= 1u) {
            visibleChunkPackets = buildVisibleChunkPackets(0u, entries.size());
        } else {
            const size_t sliceSize = (entries.size() + workerCount - 1u) / workerCount;
            std::vector<std::future<std::vector<VisibleChunkPacket>>> futures;
            futures.reserve(workerCount - 1u);

            size_t sliceStart = 0u;
            for (size_t worker = 0u; worker + 1u < workerCount; ++worker) {
                const size_t sliceEnd = std::min(entries.size(), sliceStart + sliceSize);
                futures.push_back(std::async(
                    std::launch::async, buildVisibleChunkPackets, sliceStart, sliceEnd
                ));
                sliceStart = sliceEnd;
            }

            visibleChunkPackets = buildVisibleChunkPackets(sliceStart, entries.size());
            for (auto &future : futures) {
                std::vector<VisibleChunkPacket> workerPackets = future.get();
                if (!workerPackets.empty()) {
                    visibleChunkPackets.insert(
                        visibleChunkPackets.end(),
                        std::make_move_iterator(workerPackets.begin()),
                        std::make_move_iterator(workerPackets.end())
                    );
                }
            }
        }
    }

    frameData.modelMatrices.reserve(frameData.modelMatrices.size() + visibleChunkPackets.size());
    frameData.indirectCommands.reserve(frameData.indirectCommands.size() + visibleChunkPackets.size());
    frameData.indirectBatches.reserve(frameData.indirectBatches.size() + visibleChunkPackets.size());

    const bool tryChunkSuperbatch = chunkSuperbatchEnabled() && !visibleChunkPackets.empty();
    if (tryChunkSuperbatch) {
        size_t totalVertices = 0;
        size_t totalIndices = 0;
        bool superbatchCompatible = true;
        for (const VisibleChunkPacket &packet : visibleChunkPackets) {
            if (!packet.mesh->isPackedVoxelGeometry() || packet.mesh->getPackedVertices().empty() ||
                packet.mesh->getPackedIndices().empty()) {
                superbatchCompatible = false;
                break;
            }
            totalVertices += packet.mesh->getPackedVertices().size();
            totalIndices += packet.mesh->getPackedIndices().size();
        }

        if (superbatchCompatible && totalVertices <= static_cast<size_t>(std::numeric_limits<int32_t>::max()) &&
            totalIndices <= static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            frameData.chunkSuperbatchEnabled = true;
            frameData.chunkSuperbatchTexture = &atlasTexture;
            frameData.chunkSuperbatchVertices.reserve(totalVertices);
            frameData.chunkSuperbatchIndices.reserve(totalIndices);
        }
    }

    for (const VisibleChunkPacket &packet : visibleChunkPackets) {
        const uint32_t modelIndex = static_cast<uint32_t>(frameData.modelMatrices.size());
        frameData.modelMatrices.push_back(packet.model);

        const uint32_t commandIndex = static_cast<uint32_t>(frameData.indirectCommands.size());
        IndexedIndirectCommand command{};
        command.indexCount = packet.mesh->getIndexCount();
        command.instanceCount = 1;
        command.firstIndex = 0;
        command.vertexOffset = 0;
        command.firstInstance = modelIndex;

        if (frameData.chunkSuperbatchEnabled) {
            const std::vector<VkMesh::PackedVoxelVertex> &meshVertices =
                packet.mesh->getPackedVertices();
            const std::vector<uint16_t> &meshIndices = packet.mesh->getPackedIndices();
            const uint32_t vertexOffset =
                static_cast<uint32_t>(frameData.chunkSuperbatchVertices.size());
            const uint32_t firstIndex =
                static_cast<uint32_t>(frameData.chunkSuperbatchIndices.size());

            for (const VkMesh::PackedVoxelVertex &v : meshVertices) {
                frameData.chunkSuperbatchVertices.push_back(PackedVoxelVertexGpu{v.low, v.high});
            }
            frameData.chunkSuperbatchIndices.insert(
                frameData.chunkSuperbatchIndices.end(), meshIndices.begin(), meshIndices.end()
            );

            command.indexCount = static_cast<uint32_t>(meshIndices.size());
            command.firstIndex = firstIndex;
            command.vertexOffset = static_cast<int32_t>(vertexOffset);
        }

        frameData.indirectCommands.push_back(command);

        if (!frameData.chunkSuperbatchEnabled) {
            RenderIndirectBatch batch{};
            batch.mesh = packet.mesh;
            batch.texture = &atlasTexture;
            batch.firstCommand = commandIndex;
            batch.commandCount = 1;
            frameData.indirectBatches.push_back(batch);
        }
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
