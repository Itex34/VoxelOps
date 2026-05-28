#include "WorldInteractionSystem.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/items/PlaceableBlockMapping.hpp"
#include "../../Shared/player/BlockPlace.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <iostream>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    bool IsMouseButtonDown(uint8_t button) {
        return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
    }

    std::optional<BlockID> BlockTypeFromInventoryItemId(uint16_t itemId) {
        const std::optional<uint8_t> mappedBlockId = PlaceableBlockMapping::blockIdFromItemId(itemId);
        if (!mappedBlockId.has_value()) {
            return std::nullopt;
        }
        if (*mappedBlockId >= static_cast<uint8_t>(BlockID::COUNT)) {
            return std::nullopt;
        }
        return static_cast<BlockID>(*mappedBlockId);
    }

    bool IsAirBlock(ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        return chunkManager.getBlockGlobal(worldPos.x, worldPos.y, worldPos.z) == BlockID::Air;
    }

    bool IsChunkLoadedAtWorldPos(const ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
        return chunkManager.inBounds(chunkPos) && chunkManager.hasChunkLoaded(chunkPos);
    }

    bool IsLoadedAirBlock(ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        if (!IsChunkLoadedAtWorldPos(chunkManager, worldPos)) {
            return false;
        }
        return IsAirBlock(chunkManager, worldPos);
    }

    bool IsTrueForwardEdgeBlock(
        ChunkManager &chunkManager, const glm::ivec3 &worldPos, const glm::ivec3 &lookForward
    ) {
        if (lookForward == glm::ivec3(0)) {
            return false;
        }
        const glm::ivec3 forwardPos = worldPos + lookForward;
        // Only treat loaded + empty forward space as a valid edge trigger.
        // This avoids false positives from unloaded chunks being reported as Air.
        return IsLoadedAirBlock(chunkManager, forwardPos);
    }

    glm::ivec3 QuantizeHorizontalForward(const glm::vec3 &lookDirection) {
        if (!std::isfinite(lookDirection.x) || !std::isfinite(lookDirection.z)) {
            return glm::ivec3(0, 0, 1);
        }
        const float absX = std::fabs(lookDirection.x);
        const float absZ = std::fabs(lookDirection.z);
        if (absX < 1e-5f && absZ < 1e-5f) {
            return glm::ivec3(0, 0, 1);
        }
        if (absX >= absZ) {
            return glm::ivec3((lookDirection.x >= 0.0f) ? 1 : -1, 0, 0);
        }
        return glm::ivec3(0, 0, (lookDirection.z >= 0.0f) ? 1 : -1);
    }

    void MarkChunkAndEdgeNeighborsDirty(ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
        const glm::ivec3 localPos = chunkManager.worldToLocalPos(worldPos);
        chunkManager.markChunkDirtyHighPriority(chunkPos);
        if (localPos.x == 0)
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(-1, 0, 0));
        if (localPos.x == CHUNK_SIZE - 1)
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(1, 0, 0));
        if (localPos.y == 0)
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, -1, 0));
        if (localPos.y == CHUNK_SIZE - 1)
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, 1, 0));
        if (localPos.z == 0)
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, 0, -1));
        if (localPos.z == CHUNK_SIZE - 1)
            chunkManager.markChunkDirtyHighPriority(chunkPos + glm::ivec3(0, 0, 1));
    }

    std::vector<glm::ivec3>
    CollectChunkAndEdgeNeighbors(const ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunks;
        const auto tryInsert = [&](const glm::ivec3 &chunkPos) {
            if (chunkManager.inBounds(chunkPos)) {
                chunks.insert(chunkPos);
            }
        };
        const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
        const glm::ivec3 localPos = chunkManager.worldToLocalPos(worldPos);

        tryInsert(chunkPos);
        if (localPos.x == 0)
            tryInsert(chunkPos + glm::ivec3(-1, 0, 0));
        if (localPos.x == CHUNK_SIZE - 1)
            tryInsert(chunkPos + glm::ivec3(1, 0, 0));
        if (localPos.y == 0)
            tryInsert(chunkPos + glm::ivec3(0, -1, 0));
        if (localPos.y == CHUNK_SIZE - 1)
            tryInsert(chunkPos + glm::ivec3(0, 1, 0));
        if (localPos.z == 0)
            tryInsert(chunkPos + glm::ivec3(0, 0, -1));
        if (localPos.z == CHUNK_SIZE - 1)
            tryInsert(chunkPos + glm::ivec3(0, 0, 1));

        std::vector<glm::ivec3> out;
        out.reserve(chunks.size());
        for (const glm::ivec3 &c : chunks) {
            out.push_back(c);
        }
        return out;
    }
} // namespace

void WorldInteractionSystem::update(Runtime &runtime, const WorldInteractionSystemContext &ctx) {
    bool localWasWorldInteractPressed = false;
    bool &wasWorldInteractPressed = (ctx.wasWorldInteractPressed != nullptr)
                                        ? *ctx.wasWorldInteractPressed
                                        : localWasWorldInteractPressed;
    const bool leftPressed = IsMouseButtonDown(SDL_BUTTON_LEFT);
    const bool leftClicked = leftPressed && !m_wasBlockPlaceActionPressed;
    m_wasBlockPlaceActionPressed = leftPressed;

    const bool rightPressed = IsMouseButtonDown(SDL_BUTTON_RIGHT);
    const bool rightClicked = rightPressed && !wasWorldInteractPressed;
    wasWorldInteractPressed = rightPressed;

    if (!runtime.combat.localPlayerAlive || GameData::cursorEnabled ||
        AppHelpers::IsImGuiTextInputActive()) {
        return;
    }

    Slot activeSlot{};
    bool hasActiveItem = false;
    ItemType activeItemType = ItemType::Other;
    std::optional<BlockID> activeBlockType = std::nullopt;
    if (runtime.ui.inventoryUi && runtime.ui.inventoryUi->hasSnapshot() &&
        runtime.combat.activeHotbarSlot < static_cast<uint16_t>(kHotbarSlots)) {
        activeSlot = runtime.ui.inventoryUi->slots()[runtime.combat.activeHotbarSlot];
        hasActiveItem =
            !Inventory::IsEmpty(activeSlot) && Inventory::IsValidItemId(activeSlot.itemId);
        if (hasActiveItem) {
            activeItemType = Items::ItemDatabase[activeSlot.itemId].type;
            activeBlockType = BlockTypeFromInventoryItemId(activeSlot.itemId);
        }
    }

    if (hasActiveItem && activeItemType == ItemType::Gun) {
        return;
    }
    if (hasActiveItem && activeItemType == ItemType::Block && activeBlockType.has_value()) {
        if (rightClicked) {
            runtime.world.blockPlaceMode = BlockPlace::NextMode(runtime.world.blockPlaceMode);
            std::cout << "[Build] Mode: " << BlockPlace::ToString(runtime.world.blockPlaceMode)
                      << "\n";
            return;
        }
        if (!leftClicked) {
            return;
        }
    } else if (!rightClicked) {
        return;
    }

    Ray ray(runtime.gameplay.player->getCamera().position, runtime.gameplay.player->getCamera().front);
    const RayResult hitResult = runtime.gameplay.rayManager.rayHasBlockIntersectSingle(
        ray, *runtime.gameplay.chunkManager, runtime.gameplay.player->maxReach
    );
    if (!hitResult.hit) {
        return;
    }

    if (hasActiveItem && activeItemType == ItemType::Block && activeBlockType.has_value()) {
        glm::ivec3 placementAdjacentAnchor = hitResult.adjacentAirBlockWorld;
        bool floorTopFaceForwardAssist = false;
        if (runtime.world.blockPlaceMode == BlockPlace::BlockMode::Floor) {
            glm::ivec3 faceNormal = hitResult.adjacentAirBlockWorld - hitResult.hitBlockWorld;
            const glm::ivec3 lookForward =
                QuantizeHorizontalForward(runtime.gameplay.player->getCamera().front);
            const bool isForwardEdgeBlock = IsTrueForwardEdgeBlock(
                *runtime.gameplay.chunkManager, hitResult.hitBlockWorld, lookForward
            );
            // Bridge helper: only trigger on true edge blocks when the hit face is +Y.
            if (isForwardEdgeBlock && faceNormal == glm::ivec3(0, 1, 0)) {
                floorTopFaceForwardAssist = true;
            }
        }

        const std::vector<glm::ivec3> candidatePositions = BlockPlace::BuildPlacementPositions(
            runtime.world.blockPlaceMode,
            hitResult.hitBlockWorld,
            placementAdjacentAnchor,
            runtime.gameplay.player->getCamera().front,
            floorTopFaceForwardAssist
        );
        if (candidatePositions.empty()) {
            return;
        }

        struct PlacementEdit {
            glm::ivec3 worldPos{0};
            BlockID oldBlock = BlockID::Air;
        };

        std::vector<PlacementEdit> acceptedEdits;
        acceptedEdits.reserve(candidatePositions.size());
        for (const glm::ivec3 &placePos : candidatePositions) {
            const BlockID previousBlock =
                runtime.gameplay.chunkManager->getBlockGlobal(placePos.x, placePos.y, placePos.z);
            if (previousBlock != BlockID::Air) {
                continue;
            }
            acceptedEdits.push_back(PlacementEdit{placePos, previousBlock});
            if (acceptedEdits.size() >= kMaxBlockPlaceEditsPerRequest) {
                break;
            }
        }
        if (acceptedEdits.empty()) {
            return;
        }

        if (!runtime.network.clientNet.IsConnected()) {
            for (const PlacementEdit &edit : acceptedEdits) {
                runtime.gameplay.chunkManager->setBlockGlobal(
                    edit.worldPos.x, edit.worldPos.y, edit.worldPos.z, *activeBlockType
                );
                MarkChunkAndEdgeNeighborsDirty(*runtime.gameplay.chunkManager, edit.worldPos);
            }
            return;
        }

        BlockPlaceRequest request{};
        request.requestId = runtime.world.nextBlockPlaceRequestId++;
        request.edits.reserve(acceptedEdits.size());
        for (const PlacementEdit &edit : acceptedEdits) {
            request.edits.push_back(
                BlockPlaceEdit{
                    edit.worldPos.x,
                    edit.worldPos.y,
                    edit.worldPos.z,
                    static_cast<uint8_t>(*activeBlockType)
                }
            );
        }

        if (runtime.network.clientNet.SendBlockPlaceRequest(request)) {
            for (const PlacementEdit &edit : acceptedEdits) {
                runtime.gameplay.chunkManager->setBlockGlobal(
                    edit.worldPos.x, edit.worldPos.y, edit.worldPos.z, *activeBlockType
                );
                MarkChunkAndEdgeNeighborsDirty(*runtime.gameplay.chunkManager, edit.worldPos);
            }

            RuntimeWorldState::PendingBlockPlaceRequest pending{};
            pending.createdAt = AppHelpers::GetTimeSeconds();
            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> pendingChunks;
            for (const PlacementEdit &edit : acceptedEdits) {
                const std::vector<glm::ivec3> chunks = CollectChunkAndEdgeNeighbors(
                    *runtime.gameplay.chunkManager, edit.worldPos
                );
                for (const glm::ivec3 &chunk : chunks) {
                    pendingChunks.insert(chunk);
                }
                pending.edits.push_back(
                    RuntimeWorldState::PendingBlockPlaceEdit{
                        edit.worldPos,
                        static_cast<uint8_t>(edit.oldBlock),
                        static_cast<uint8_t>(*activeBlockType)
                    }
                );
            }
            pending.affectedChunks.reserve(pendingChunks.size());
            for (const glm::ivec3 &chunk : pendingChunks) {
                pending.affectedChunks.push_back(chunk);
            }
            runtime.world.pendingBlockPlaceRequests[request.requestId] = std::move(pending);
        }
        return;
    }

    if (!runtime.network.clientNet.IsConnected()) {
        runtime.gameplay.chunkManager->playerBreakBlockAt(hitResult.hitBlockWorld);
        return;
    }

    const glm::ivec3 breakPos = hitResult.hitBlockWorld;
    const BlockID previousBlock =
        runtime.gameplay.chunkManager->getBlockGlobal(breakPos.x, breakPos.y, breakPos.z);
    if (previousBlock == BlockID::Air) {
        return;
    }

    BlockBreakRequest request{};
    request.requestId = runtime.world.nextBlockBreakRequestId++;
    request.edits.push_back(BlockBreakEdit{breakPos.x, breakPos.y, breakPos.z});

    if (runtime.network.clientNet.SendBlockBreakRequest(request)) {
        runtime.gameplay.chunkManager->playerBreakBlockAt(breakPos);

        RuntimeWorldState::PendingBlockBreakRequest pending{};
        pending.createdAt = AppHelpers::GetTimeSeconds();
        pending.affectedChunks =
            CollectChunkAndEdgeNeighbors(*runtime.gameplay.chunkManager, breakPos);
        pending.edits.push_back(
            RuntimeWorldState::PendingBlockBreakEdit{breakPos, static_cast<uint8_t>(previousBlock)}
        );
        runtime.world.pendingBlockBreakRequests[request.requestId] = std::move(pending);
    }
}
