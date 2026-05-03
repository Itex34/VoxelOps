#include "WorldInteractionSystem.hpp"

#include "../application/AppHelpers.hpp"
#include "../data/GameData.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    bool IsMouseButtonDown(uint8_t button) {
        return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
    }

    std::optional<BlockID> BlockTypeFromInventoryItemId(uint16_t itemId) {
        switch (itemId) {
        case static_cast<uint16_t>(ITEM_DIRT_BLOCK):
            return BlockID::Dirt;
        case static_cast<uint16_t>(ITEM_SAPPHIRE_BLOCK):
            return BlockID::SapphireBlock;
        default:
            return std::nullopt;
        }
    }

    void MarkChunkAndEdgeNeighborsDirty(ChunkManager &chunkManager, const glm::ivec3 &worldPos) {
        const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
        const glm::ivec3 localPos = chunkManager.worldToLocalPos(worldPos);
        chunkManager.markChunkDirty(chunkPos);
        if (localPos.x == 0)
            chunkManager.markChunkDirty(chunkPos + glm::ivec3(-1, 0, 0));
        if (localPos.x == CHUNK_SIZE - 1)
            chunkManager.markChunkDirty(chunkPos + glm::ivec3(1, 0, 0));
        if (localPos.y == 0)
            chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, -1, 0));
        if (localPos.y == CHUNK_SIZE - 1)
            chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, 1, 0));
        if (localPos.z == 0)
            chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, 0, -1));
        if (localPos.z == CHUNK_SIZE - 1)
            chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, 0, 1));
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
    const bool rightPressed = IsMouseButtonDown(SDL_BUTTON_RIGHT);
    const bool rightClicked = rightPressed && !wasWorldInteractPressed;
    wasWorldInteractPressed = rightPressed;

    if (!runtime.combat.localPlayerAlive || GameData::cursorEnabled ||
        AppHelpers::IsImGuiTextInputActive()) {
        return;
    }

    if (!rightClicked) {
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

    Ray ray(runtime.gameplay.player->getCamera().position, runtime.gameplay.player->getCamera().front);
    const RayResult hitResult = runtime.gameplay.rayManager.rayHasBlockIntersectSingle(
        ray, *runtime.gameplay.chunkManager, runtime.gameplay.player->maxReach
    );
    if (!hitResult.hit) {
        return;
    }

    if (hasActiveItem && activeItemType == ItemType::Block && activeBlockType.has_value()) {
        const glm::ivec3 placePos = hitResult.adjacentAirBlockWorld;
        const BlockID previousBlock =
            runtime.gameplay.chunkManager->getBlockGlobal(placePos.x, placePos.y, placePos.z);
        if (previousBlock != BlockID::Air) {
            return;
        }

        if (!runtime.network.clientNet.IsConnected()) {
            runtime.gameplay.chunkManager->setBlockGlobal(
                placePos.x, placePos.y, placePos.z, *activeBlockType
            );
            MarkChunkAndEdgeNeighborsDirty(*runtime.gameplay.chunkManager, placePos);
            return;
        }

        BlockPlaceRequest request{};
        request.requestId = runtime.world.nextBlockPlaceRequestId++;
        request.edits.push_back(
            BlockPlaceEdit{
                placePos.x, placePos.y, placePos.z, static_cast<uint8_t>(*activeBlockType)
            }
        );

        if (runtime.network.clientNet.SendBlockPlaceRequest(request)) {
            runtime.gameplay.chunkManager->setBlockGlobal(
                placePos.x, placePos.y, placePos.z, *activeBlockType
            );
            MarkChunkAndEdgeNeighborsDirty(*runtime.gameplay.chunkManager, placePos);

            RuntimeWorldState::PendingBlockPlaceRequest pending{};
            pending.createdAt = AppHelpers::GetTimeSeconds();
            pending.affectedChunks =
                CollectChunkAndEdgeNeighbors(*runtime.gameplay.chunkManager, placePos);
            pending.edits.push_back(
                RuntimeWorldState::PendingBlockPlaceEdit{
                    placePos,
                    static_cast<uint8_t>(previousBlock),
                    static_cast<uint8_t>(*activeBlockType)
                }
            );
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
