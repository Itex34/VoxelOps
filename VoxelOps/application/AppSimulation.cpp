#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "FrameOrchestrator.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>

using namespace AppHelpers;

namespace {
bool IsScancodeDown(SDL_Scancode scancode) {
    int keyCount = 0;
    const bool *keys = SDL_GetKeyboardState(&keyCount);
    return keys != nullptr && scancode < keyCount && keys[scancode];
}

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

std::vector<glm::ivec3> CollectChunkAndEdgeNeighbors(const ChunkManager &chunkManager,
                                                     const glm::ivec3 &worldPos) {
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

void App::pollEvents(Runtime &runtime) {
    SDL_Event event;
    const SDL_WindowID windowId = (m_Window != nullptr) ? SDL_GetWindowID(m_Window) : 0;
    while (SDL_PollEvent(&event)) {
        if (runtime.debugUi) {
            runtime.debugUi->processEvent(event);
        }

        switch (event.type) {
        case SDL_EVENT_QUIT:
            m_ShouldQuit = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == windowId) {
                m_ShouldQuit = true;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            if (event.window.windowID == windowId && runtime.inputCallbacks) {
                runtime.inputCallbacks->framebuffer_size_callback(m_Window, event.window.data1,
                                                                  event.window.data2);
                runtime.renderer->onWindowResized(event.window.data1, event.window.data2);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.windowID == windowId && runtime.inputCallbacks) {
                runtime.inputCallbacks->mouse_motion_callback(m_Window, event.motion.x,
                                                              event.motion.y, event.motion.xrel,
                                                              event.motion.yrel, m_UseDebugCamera);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.windowID == windowId && runtime.inputCallbacks) {
                runtime.inputCallbacks->mouse_button_callback(
                    m_Window, event.button.button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            break;
        default:
            break;
        }
    }
}

void App::updateDebugCamera(Runtime &runtime) {
    if (m_UseDebugCamera && m_Window && SDL_GetWindowRelativeMouseMode(m_Window)) {
        float mouseDx = 0.0f;
        float mouseDy = 0.0f;
        SDL_GetRelativeMouseState(&mouseDx, &mouseDy);
        runtime.inputLook.yaw += (mouseDx * 0.1f);
        runtime.inputLook.pitch -= (mouseDy * 0.1f);
        runtime.inputLook.pitch = glm::clamp(runtime.inputLook.pitch, -89.0f, 89.0f);
    } else {
        float mouseX = 0.0f;
        float mouseY = 0.0f;
        SDL_GetMouseState(&mouseX, &mouseY);
        runtime.inputLook.xpos = mouseX;
        runtime.inputLook.ypos = mouseY;
    }

    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    glm::vec3 moveDir(0.0f);
    if (!keyboardBlockedByUi) {
        if (IsScancodeDown(SDL_SCANCODE_U))
            moveDir += runtime.debugCamera.XZfront;
        if (IsScancodeDown(SDL_SCANCODE_J))
            moveDir -= runtime.debugCamera.XZfront;
        if (IsScancodeDown(SDL_SCANCODE_H))
            moveDir -=
                glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
        if (IsScancodeDown(SDL_SCANCODE_K))
            moveDir +=
                glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
        if (IsScancodeDown(SDL_SCANCODE_RALT))
            moveDir += runtime.debugCamera.up;
        if (IsScancodeDown(SDL_SCANCODE_V))
            moveDir -= runtime.debugCamera.up;
    }

    if (glm::length(moveDir) > 0.0f) {
        moveDir = glm::normalize(moveDir);
    }
    runtime.debugCamera.position += moveDir * 10.0f * static_cast<float>(GameData::deltaTime);

    if (m_UseDebugCamera && (!m_Window || !SDL_GetWindowRelativeMouseMode(m_Window))) {
        const double xoffset = runtime.inputLook.xpos - runtime.inputLook.lastX;
        const double yoffset = runtime.inputLook.ypos - runtime.inputLook.lastY;
        runtime.inputLook.lastX = runtime.inputLook.xpos;
        runtime.inputLook.lastY = runtime.inputLook.ypos;

        runtime.inputLook.yaw += static_cast<float>(xoffset * 0.1);
        runtime.inputLook.pitch -= static_cast<float>(yoffset * 0.1);
        runtime.inputLook.pitch = glm::clamp(runtime.inputLook.pitch, -89.0f, 89.0f);
    }

    runtime.debugCamera.updateRotation(runtime.inputLook.yaw, runtime.inputLook.pitch);
}

void App::updateToggleStates(Runtime &runtime) {
    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    const bool textInputBlocked =
        (ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().WantTextInput;
    const auto refreshCursorState = [&]() {
        GameData::cursorEnabled = m_ForceCursorEnabled || m_ShowDebugUi || m_ShowInventoryUi ||
                                  !runtime.clientNet.IsConnected();
        applyMouseInputModes();
    };

    const bool isF1Pressed = IsScancodeDown(SDL_SCANCODE_F1);
    if (!keyboardBlockedByUi && isF1Pressed && !m_WasF1Pressed) {
        m_UseDebugCamera = !m_UseDebugCamera;
    }
    m_WasF1Pressed = isF1Pressed;

    const bool isTPressed = IsScancodeDown(SDL_SCANCODE_T);
    if (!keyboardBlockedByUi && isTPressed && !m_WasTPressed) {
        m_ToggleWireframe = !m_ToggleWireframe;
    }
    m_WasTPressed = isTPressed;

    const bool isF2Pressed = IsScancodeDown(SDL_SCANCODE_F2);
    if (!keyboardBlockedByUi && isF2Pressed && !m_WasF2Pressed) {
        m_ToggleChunkBorders = !m_ToggleChunkBorders;
    }
    m_WasF2Pressed = isF2Pressed;

    const bool isF3Pressed = IsScancodeDown(SDL_SCANCODE_F3);
    if (!keyboardBlockedByUi && isF3Pressed && !m_WasF3Pressed) {
        m_ToggleDebugFrustum = !m_ToggleDebugFrustum;
    }
    m_WasF3Pressed = isF3Pressed;

    const bool isF10Pressed = IsScancodeDown(SDL_SCANCODE_F10);
    if (!keyboardBlockedByUi && isF10Pressed && !m_WasF10Pressed) {
        m_ShowDebugUi = !m_ShowDebugUi;
        if (runtime.debugUi) {
            runtime.debugUi->setVisible(m_ShowDebugUi);
        }
        refreshCursorState();
    }
    m_WasF10Pressed = isF10Pressed;

    const bool isXPressed = IsScancodeDown(SDL_SCANCODE_X);
    if (!textInputBlocked && isXPressed && !m_WasXPressed) {
        m_ShowInventoryUi = !m_ShowInventoryUi;
        if (runtime.inventoryUi) {
            runtime.inventoryUi->setVisible(m_ShowInventoryUi);
        }
        refreshCursorState();
    }
    m_WasXPressed = isXPressed;

    const bool isEscapePressed = IsScancodeDown(SDL_SCANCODE_ESCAPE);
    if (!textInputBlocked && isEscapePressed && !m_WasEscapePressed) {
        m_ForceCursorEnabled = !m_ForceCursorEnabled;
        refreshCursorState();
    }
    m_WasEscapePressed = isEscapePressed;

    const bool canRecaptureCursor = runtime.clientNet.IsConnected() && !m_ShowDebugUi &&
                                    !m_ShowInventoryUi && m_ForceCursorEnabled;
    const bool primaryMouseDown = IsMouseButtonDown(SDL_BUTTON_LEFT);
    if (canRecaptureCursor && primaryMouseDown && !textInputBlocked) {
        m_ForceCursorEnabled = false;
        refreshCursorState();
    }
}


void App::processWorldInteraction(Runtime &runtime) {
    const bool rightPressed = IsMouseButtonDown(SDL_BUTTON_RIGHT);
    const bool rightClicked = rightPressed && !m_WasWorldInteractPressed;
    m_WasWorldInteractPressed = rightPressed;

    if (!runtime.combat.localPlayerAlive || GameData::cursorEnabled || IsImGuiTextInputActive()) {
        return;
    }

    if (!rightClicked) {
        return;
    }

    Slot activeSlot{};
    bool hasActiveItem = false;
    ItemType activeItemType = ItemType::Other;
    std::optional<BlockID> activeBlockType = std::nullopt;
    if (runtime.inventoryUi && runtime.inventoryUi->hasSnapshot() &&
        runtime.combat.activeHotbarSlot < static_cast<uint16_t>(kHotbarSlots)) {
        activeSlot = runtime.inventoryUi->slots()[runtime.combat.activeHotbarSlot];
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

    Ray ray(runtime.player->getCamera().position, runtime.player->getCamera().front);
    const RayResult hitResult = runtime.rayManager.rayHasBlockIntersectSingle(
        ray, *runtime.chunkManager, runtime.player->maxReach);
    if (!hitResult.hit) {
        return;
    }

    if (hasActiveItem && activeItemType == ItemType::Block && activeBlockType.has_value()) {
        const glm::ivec3 placePos = hitResult.adjacentAirBlockWorld;
        const BlockID previousBlock =
            runtime.chunkManager->getBlockGlobal(placePos.x, placePos.y, placePos.z);
        if (previousBlock != BlockID::Air) {
            return;
        }

        if (!runtime.clientNet.IsConnected()) {
            runtime.chunkManager->setBlockGlobal(placePos.x, placePos.y, placePos.z,
                                                 *activeBlockType);
            MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, placePos);
            return;
        }

        BlockPlaceRequest request{};
        request.requestId = runtime.nextBlockPlaceRequestId++;
        request.edits.push_back(BlockPlaceEdit{placePos.x, placePos.y, placePos.z,
                                               static_cast<uint8_t>(*activeBlockType)});

        if (runtime.clientNet.SendBlockPlaceRequest(request)) {
            runtime.chunkManager->setBlockGlobal(placePos.x, placePos.y, placePos.z,
                                                 *activeBlockType);
            MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, placePos);

            Runtime::PendingBlockPlaceRequest pending{};
            pending.createdAt = GetTimeSeconds();
            pending.affectedChunks = CollectChunkAndEdgeNeighbors(*runtime.chunkManager, placePos);
            pending.edits.push_back(
                Runtime::PendingBlockPlaceEdit{placePos, static_cast<uint8_t>(previousBlock),
                                               static_cast<uint8_t>(*activeBlockType)});
            runtime.pendingBlockPlaceRequests[request.requestId] = std::move(pending);
        }
        return;
    }

    if (!runtime.clientNet.IsConnected()) {
        runtime.chunkManager->playerBreakBlockAt(hitResult.hitBlockWorld);
        return;
    }

    const glm::ivec3 breakPos = hitResult.hitBlockWorld;
    const BlockID previousBlock =
        runtime.chunkManager->getBlockGlobal(breakPos.x, breakPos.y, breakPos.z);
    if (previousBlock == BlockID::Air) {
        return;
    }

    BlockBreakRequest request{};
    request.requestId = runtime.nextBlockBreakRequestId++;
    request.edits.push_back(BlockBreakEdit{breakPos.x, breakPos.y, breakPos.z});

    if (runtime.clientNet.SendBlockBreakRequest(request)) {
        runtime.chunkManager->playerBreakBlockAt(breakPos);

        Runtime::PendingBlockBreakRequest pending{};
        pending.createdAt = GetTimeSeconds();
        pending.affectedChunks = CollectChunkAndEdgeNeighbors(*runtime.chunkManager, breakPos);
        pending.edits.push_back(
            Runtime::PendingBlockBreakEdit{breakPos, static_cast<uint8_t>(previousBlock)});
        runtime.pendingBlockBreakRequests[request.requestId] = std::move(pending);
    }
}

void App::processShooting(Runtime &runtime) {
    if (!runtime.combat.localPlayerAlive) {
        return;
    }

    if (GameData::cursorEnabled || m_UseDebugCamera) {
        return;
    }
    if (!runtime.clientNet.IsConnected()) {
        return;
    }
    if (!runtime.combat.equippedGun) {
        return;
    }

    const bool triggerPressed = IsMouseButtonDown(SDL_BUTTON_LEFT);
    if (!triggerPressed) {
        return;
    }

    const double now = GetTimeSeconds();
    if ((now - runtime.combat.lastShootSendTime) < runtime.combat.shootSendInterval) {
        return;
    }

    const Camera &cam = runtime.player->getCamera();
    const float dirLenSq = glm::dot(cam.front, cam.front);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        return;
    }

    const glm::vec3 shootDir = glm::normalize(cam.front);
    const glm::vec3 shootPos = cam.position;
    const uint32_t shotId = runtime.combat.nextClientShotId++;
    const uint32_t clientTick = runtime.hasAppliedServerTick ? runtime.lastAppliedServerTick : 0u;
    const uint32_t seed = shotId ^ (clientTick * 2654435761u);

    if (runtime.clientNet.SendShootRequest(shotId, clientTick, runtime.combat.equippedGun->getWeaponId(),
                                           shootPos, shootDir, seed, 0)) {
        runtime.combat.lastShootSendTime = now;
    }
}

void App::processChunkStreaming(Runtime &runtime, bool prioritizeMovement) {
    constexpr double kChunkResyncCooldownSec = 0.25;
    static std::unordered_map<glm::ivec3, double, IVec3Hash> s_chunkResyncCooldownUntil;

    const auto requestChunkResync = [&](const glm::ivec3 &chunkPos, bool force) {
        const double nowSec = GetTimeSeconds();
        auto it = s_chunkResyncCooldownUntil.find(chunkPos);
        if (!force && it != s_chunkResyncCooldownUntil.end() && nowSec < it->second) {
            return;
        }
        s_chunkResyncCooldownUntil[chunkPos] = nowSec + kChunkResyncCooldownSec;
        if (!runtime.clientNet.SendChunkResyncRequest(chunkPos)) {
            std::cerr << "[chunk/resync] failed to request full chunk (" << chunkPos.x << ","
                      << chunkPos.y << "," << chunkPos.z << ")\n";
        }
    };

    const int64_t chunkApplyBudgetUs = prioritizeMovement
                                           ? Runtime::ChunkApplyBudgetUsUnderInputPressure
                                           : Runtime::ChunkApplyBudgetUs;
    const auto chunkApplyStart = std::chrono::steady_clock::now();
    const auto withinChunkApplyBudget = [&]() -> bool {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - chunkApplyStart)
                                   .count();
        return elapsedUs < chunkApplyBudgetUs;
    };
    size_t chunkDataApplied = 0;

    ChunkData chunkData;
    while (chunkDataApplied < Runtime::MaxChunkDataApplyPerFrame && withinChunkApplyBudget() &&
           runtime.clientNet.PopChunkData(chunkData)) {
        runtime.chunkManager->applyNetworkChunkData(chunkData);
        ++chunkDataApplied;
    }

    ChunkDelta chunkDelta;
    size_t chunkDeltaApplied = 0;
    while (chunkDeltaApplied < Runtime::MaxChunkDeltaApplyPerFrame && withinChunkApplyBudget() &&
           runtime.clientNet.PopChunkDelta(chunkDelta)) {
        const NetworkChunkDeltaApplyResult deltaResult =
            runtime.chunkManager->applyNetworkChunkDelta(chunkDelta);
        if (deltaResult == NetworkChunkDeltaApplyResult::MissingBaseChunk ||
            deltaResult == NetworkChunkDeltaApplyResult::VersionGap) {
            requestChunkResync(glm::ivec3(chunkDelta.chunkX, chunkDelta.chunkY, chunkDelta.chunkZ),
                               false);
        }
        ++chunkDeltaApplied;
    }

    ChunkUnload chunkUnload;
    size_t chunkUnloadApplied = 0;
    while (chunkUnloadApplied < Runtime::MaxChunkUnloadApplyPerFrame && withinChunkApplyBudget() &&
           runtime.clientNet.PopChunkUnload(chunkUnload)) {
        runtime.chunkManager->applyNetworkChunkUnload(chunkUnload);
        ++chunkUnloadApplied;
    }

    BlockPlaceResult blockPlaceResult;
    size_t blockPlaceResultsApplied = 0;
    while (blockPlaceResultsApplied < Runtime::MaxBlockPlaceResultsPerFrame &&
           runtime.clientNet.PopBlockPlaceResult(blockPlaceResult)) {
        auto pendingIt = runtime.pendingBlockPlaceRequests.find(blockPlaceResult.requestId);
        if (blockPlaceResult.accepted == 0) {
            if (pendingIt != runtime.pendingBlockPlaceRequests.end()) {
                for (const Runtime::PendingBlockPlaceEdit &edit : pendingIt->second.edits) {
                    const BlockID predictedId = static_cast<BlockID>(edit.newBlockId);
                    const BlockID rollbackId = static_cast<BlockID>(edit.oldBlockId);
                    if (runtime.chunkManager->getBlockGlobal(edit.worldPos.x, edit.worldPos.y,
                                                             edit.worldPos.z) == predictedId) {
                        runtime.chunkManager->setBlockGlobal(edit.worldPos.x, edit.worldPos.y,
                                                             edit.worldPos.z, rollbackId);
                        MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, edit.worldPos);
                    }
                }
            }

            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToResync;
            if (pendingIt != runtime.pendingBlockPlaceRequests.end()) {
                for (const glm::ivec3 &chunkPos : pendingIt->second.affectedChunks) {
                    chunksToResync.insert(chunkPos);
                }
            } else {
                for (const BlockPlaceChunkCoord &coord : blockPlaceResult.correctiveChunks) {
                    chunksToResync.insert(glm::ivec3(coord.chunkX, coord.chunkY, coord.chunkZ));
                }
            }

            for (const glm::ivec3 &chunkPos : chunksToResync) {
                requestChunkResync(chunkPos, true);
            }
        }

        if (pendingIt != runtime.pendingBlockPlaceRequests.end()) {
            runtime.pendingBlockPlaceRequests.erase(pendingIt);
        }
        ++blockPlaceResultsApplied;
    }

    const double nowSec = GetTimeSeconds();
    for (auto it = runtime.pendingBlockPlaceRequests.begin();
         it != runtime.pendingBlockPlaceRequests.end();) {
        if ((nowSec - it->second.createdAt) > 1.5) {
            for (const glm::ivec3 &chunkPos : it->second.affectedChunks) {
                requestChunkResync(chunkPos, true);
            }
            it = runtime.pendingBlockPlaceRequests.erase(it);
        } else {
            ++it;
        }
    }

    BlockBreakResult blockBreakResult;
    size_t blockBreakResultsApplied = 0;
    while (blockBreakResultsApplied < Runtime::MaxBlockBreakResultsPerFrame &&
           runtime.clientNet.PopBlockBreakResult(blockBreakResult)) {
        auto pendingIt = runtime.pendingBlockBreakRequests.find(blockBreakResult.requestId);
        if (blockBreakResult.accepted == 0) {
            if (pendingIt != runtime.pendingBlockBreakRequests.end()) {
                for (const Runtime::PendingBlockBreakEdit &edit : pendingIt->second.edits) {
                    if (runtime.chunkManager->getBlockGlobal(edit.worldPos.x, edit.worldPos.y,
                                                             edit.worldPos.z) == BlockID::Air) {
                        runtime.chunkManager->setBlockGlobal(edit.worldPos.x, edit.worldPos.y,
                                                             edit.worldPos.z,
                                                             static_cast<BlockID>(edit.oldBlockId));
                        MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, edit.worldPos);
                    }
                }
            }

            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToResync;
            if (pendingIt != runtime.pendingBlockBreakRequests.end()) {
                for (const glm::ivec3 &chunkPos : pendingIt->second.affectedChunks) {
                    chunksToResync.insert(chunkPos);
                }
            } else {
                for (const BlockBreakChunkCoord &coord : blockBreakResult.correctiveChunks) {
                    chunksToResync.insert(glm::ivec3(coord.chunkX, coord.chunkY, coord.chunkZ));
                }
            }

            for (const glm::ivec3 &chunkPos : chunksToResync) {
                requestChunkResync(chunkPos, true);
            }
        }

        if (pendingIt != runtime.pendingBlockBreakRequests.end()) {
            runtime.pendingBlockBreakRequests.erase(pendingIt);
        }
        ++blockBreakResultsApplied;
    }

    for (auto it = runtime.pendingBlockBreakRequests.begin();
         it != runtime.pendingBlockBreakRequests.end();) {
        if ((nowSec - it->second.createdAt) > 1.5) {
            for (const glm::ivec3 &chunkPos : it->second.affectedChunks) {
                requestChunkResync(chunkPos, true);
            }
            it = runtime.pendingBlockBreakRequests.erase(it);
        } else {
            ++it;
        }
    }

    const size_t maxChunkMeshBuilds = prioritizeMovement
                                          ? Runtime::MaxChunkMeshBuildsPerFrameUnderInputPressure
                                          : Runtime::MaxChunkMeshBuildsPerFrame;
    const int64_t chunkMeshBuildBudgetUs = prioritizeMovement
                                               ? Runtime::ChunkMeshBuildBudgetUsUnderInputPressure
                                               : Runtime::ChunkMeshBuildBudgetUs;
    runtime.chunkManager->updateDirtyChunks(maxChunkMeshBuilds, chunkMeshBuildBudgetUs);

    const double now = GetTimeSeconds();
    if (kEnableChunkDiagnostics && now - runtime.lastChunkCoverageLogTime >= 1.0) {
        runtime.lastChunkCoverageLogTime = now;
        const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();

        const glm::vec3 pos = runtime.player->getPosition();
        const glm::ivec3 worldPos(static_cast<int>(std::floor(pos.x)),
                                  static_cast<int>(std::floor(pos.y)),
                                  static_cast<int>(std::floor(pos.z)));
        const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
        const int viewDistance = std::max<int>(2, runtime.player->renderDistance);
        const int64_t radius2 =
            static_cast<int64_t>(viewDistance) * static_cast<int64_t>(viewDistance);
        const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
        const int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

        const auto &chunks = runtime.chunkManager->getChunks();
        size_t desired = 0;
        size_t loaded = 0;
        std::vector<glm::ivec3> missingSamples;
        missingSamples.reserve(8);
        for (int x = centerChunk.x - viewDistance; x <= centerChunk.x + viewDistance; ++x) {
            const int64_t dx = static_cast<int64_t>(x - centerChunk.x);
            const int64_t dx2 = dx * dx;
            for (int z = centerChunk.z - viewDistance; z <= centerChunk.z + viewDistance; ++z) {
                const int64_t dz = static_cast<int64_t>(z - centerChunk.z);
                if (dx2 + dz * dz > radius2) {
                    continue;
                }
                for (int y = minChunkY; y <= maxChunkY; ++y) {
                    const glm::ivec3 cp(x, y, z);
                    if (!runtime.chunkManager->inBounds(cp))
                        continue;
                    ++desired;
                    if (chunks.find(cp) != chunks.end()) {
                        ++loaded;
                    } else if (missingSamples.size() < 8) {
                        missingSamples.push_back(cp);
                    }
                }
            }
        }

        std::cerr << "[chunk/client] coverage center=(" << centerChunk.x << "," << centerChunk.y
                  << "," << centerChunk.z << ")"
                  << " viewDist=" << viewDistance << " desired=" << desired << " loaded=" << loaded
                  << " missing=" << (desired - loaded) << " queue(data/delta/unload)=("
                  << queueDepths.chunkData << "/" << queueDepths.chunkDelta << "/"
                  << queueDepths.chunkUnload << ")"
                  << " applied(data/delta/unload)=(" << chunkDataApplied << "/" << chunkDeltaApplied
                  << "/" << chunkUnloadApplied << ")\n";

        if (!missingSamples.empty()) {
            std::cerr << "[chunk/client] missing samples:";
            for (const glm::ivec3 &cp : missingSamples) {
                std::cerr << " (" << cp.x << "," << cp.y << "," << cp.z << ")";
            }
            std::cerr << "\n";
        }
    }
}

void App::processFrame(Runtime &runtime) {
    FrameOrchestrator orchestrator(*this, runtime);
    orchestrator.runFrame();
}

