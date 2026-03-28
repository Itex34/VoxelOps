#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "App.hpp"
#include "AppHelpers.hpp"

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

#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

using namespace AppHelpers;

namespace {
std::optional<GunType> GunTypeFromInventoryItemId(uint16_t itemId)
{
    switch (itemId) {
    case static_cast<uint16_t>(ITEM_PISTOL): return GunType::Pistol;
    case static_cast<uint16_t>(ITEM_SNIPER): return GunType::Sniper;
    default: return std::nullopt;
    }
}

std::optional<BlockID> BlockTypeFromInventoryItemId(uint16_t itemId)
{
    switch (itemId) {
    case static_cast<uint16_t>(ITEM_DIRT_BLOCK): return BlockID::Dirt;
    default: return std::nullopt;
    }
}

void MarkChunkAndEdgeNeighborsDirty(ChunkManager& chunkManager, const glm::ivec3& worldPos)
{
    const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
    const glm::ivec3 localPos = chunkManager.worldToLocalPos(worldPos);
    chunkManager.markChunkDirty(chunkPos);
    if (localPos.x == 0) chunkManager.markChunkDirty(chunkPos + glm::ivec3(-1, 0, 0));
    if (localPos.x == CHUNK_SIZE - 1) chunkManager.markChunkDirty(chunkPos + glm::ivec3(1, 0, 0));
    if (localPos.y == 0) chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, -1, 0));
    if (localPos.y == CHUNK_SIZE - 1) chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, 1, 0));
    if (localPos.z == 0) chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, 0, -1));
    if (localPos.z == CHUNK_SIZE - 1) chunkManager.markChunkDirty(chunkPos + glm::ivec3(0, 0, 1));
}

std::vector<glm::ivec3> CollectChunkAndEdgeNeighbors(const ChunkManager& chunkManager, const glm::ivec3& worldPos)
{
    std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunks;
    const auto tryInsert = [&](const glm::ivec3& chunkPos) {
        if (chunkManager.inBounds(chunkPos)) {
            chunks.insert(chunkPos);
        }
    };
    const glm::ivec3 chunkPos = chunkManager.worldToChunkPos(worldPos);
    const glm::ivec3 localPos = chunkManager.worldToLocalPos(worldPos);

    tryInsert(chunkPos);
    if (localPos.x == 0) tryInsert(chunkPos + glm::ivec3(-1, 0, 0));
    if (localPos.x == CHUNK_SIZE - 1) tryInsert(chunkPos + glm::ivec3(1, 0, 0));
    if (localPos.y == 0) tryInsert(chunkPos + glm::ivec3(0, -1, 0));
    if (localPos.y == CHUNK_SIZE - 1) tryInsert(chunkPos + glm::ivec3(0, 1, 0));
    if (localPos.z == 0) tryInsert(chunkPos + glm::ivec3(0, 0, -1));
    if (localPos.z == CHUNK_SIZE - 1) tryInsert(chunkPos + glm::ivec3(0, 0, 1));

    std::vector<glm::ivec3> out;
    out.reserve(chunks.size());
    for (const glm::ivec3& c : chunks) {
        out.push_back(c);
    }
    return out;
}
}

void App::updateDebugCamera(Runtime& runtime) {
    glfwGetCursorPos(m_Window, &runtime.xpos, &runtime.ypos);

    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    glm::vec3 moveDir(0.0f);
    if (!keyboardBlockedByUi) {
        if (glfwGetKey(m_Window, GLFW_KEY_U) == GLFW_PRESS) moveDir += runtime.debugCamera.XZfront;
        if (glfwGetKey(m_Window, GLFW_KEY_J) == GLFW_PRESS) moveDir -= runtime.debugCamera.XZfront;
        if (glfwGetKey(m_Window, GLFW_KEY_H) == GLFW_PRESS) moveDir -= glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
        if (glfwGetKey(m_Window, GLFW_KEY_K) == GLFW_PRESS) moveDir += glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
        if (glfwGetKey(m_Window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) moveDir += runtime.debugCamera.up;
        if (glfwGetKey(m_Window, GLFW_KEY_V) == GLFW_PRESS) moveDir -= runtime.debugCamera.up;
    }

    if (glm::length(moveDir) > 0.0f) {
        moveDir = glm::normalize(moveDir);
    }
    runtime.debugCamera.position += moveDir * 10.0f * static_cast<float>(GameData::deltaTime);

    if (m_UseDebugCamera) {
        const double xoffset = runtime.xpos - runtime.lastX;
        const double yoffset = runtime.ypos - runtime.lastY;
        runtime.lastX = runtime.xpos;
        runtime.lastY = runtime.ypos;

        runtime.yaw += static_cast<float>(xoffset * 0.1);
        runtime.pitch -= static_cast<float>(yoffset * 0.1);
        runtime.pitch = glm::clamp(runtime.pitch, -89.0f, 89.0f);
    }

    runtime.debugCamera.updateRotation(runtime.yaw, runtime.pitch);
}


void App::updateToggleStates(Runtime& runtime) {
    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    const bool textInputBlocked =
        (ImGui::GetCurrentContext() != nullptr) &&
        ImGui::GetIO().WantTextInput;
    const auto refreshCursorState = [&]() {
        GameData::cursorEnabled =
            m_ForceCursorEnabled || m_ShowDebugUi || m_ShowInventoryUi || !runtime.clientNet.IsConnected();
        applyMouseInputModes();
    };

    const bool isF1Pressed = glfwGetKey(m_Window, GLFW_KEY_F1) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF1Pressed && !m_WasF1Pressed) {
        m_UseDebugCamera = !m_UseDebugCamera;
    }
    m_WasF1Pressed = isF1Pressed;

    const bool isTPressed = glfwGetKey(m_Window, GLFW_KEY_T) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isTPressed && !m_WasTPressed) {
        m_ToggleWireframe = !m_ToggleWireframe;
    }
    m_WasTPressed = isTPressed;

    const bool isF2Pressed = glfwGetKey(m_Window, GLFW_KEY_F2) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF2Pressed && !m_WasF2Pressed) {
        m_ToggleChunkBorders = !m_ToggleChunkBorders;
    }
    m_WasF2Pressed = isF2Pressed;

    const bool isF3Pressed = glfwGetKey(m_Window, GLFW_KEY_F3) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF3Pressed && !m_WasF3Pressed) {
        m_ToggleDebugFrustum = !m_ToggleDebugFrustum;
    }
    m_WasF3Pressed = isF3Pressed;

    const bool isF10Pressed = glfwGetKey(m_Window, GLFW_KEY_F10) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF10Pressed && !m_WasF10Pressed) {
        m_ShowDebugUi = !m_ShowDebugUi;
        if (runtime.debugUi) {
            runtime.debugUi->setVisible(m_ShowDebugUi);
        }
        refreshCursorState();
    }
    m_WasF10Pressed = isF10Pressed;

    const bool isXPressed = glfwGetKey(m_Window, GLFW_KEY_X) == GLFW_PRESS;
    if (!textInputBlocked && isXPressed && !m_WasXPressed) {
        m_ShowInventoryUi = !m_ShowInventoryUi;
        if (runtime.inventoryUi) {
            runtime.inventoryUi->setVisible(m_ShowInventoryUi);
        }
        refreshCursorState();
    }
    m_WasXPressed = isXPressed;

    const bool isEscapePressed = glfwGetKey(m_Window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    if (!textInputBlocked && isEscapePressed && !m_WasEscapePressed) {
        m_ForceCursorEnabled = !m_ForceCursorEnabled;
        refreshCursorState();
    }
    m_WasEscapePressed = isEscapePressed;

    const bool canRecaptureCursor =
        runtime.clientNet.IsConnected() &&
        !m_ShowDebugUi &&
        !m_ShowInventoryUi &&
        m_ForceCursorEnabled;
    const bool primaryMouseDown = glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (canRecaptureCursor && primaryMouseDown && !textInputBlocked) {
        m_ForceCursorEnabled = false;
        refreshCursorState();
    }
}

void App::processHotbarSelection(Runtime& runtime) {
    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    for (uint16_t i = 0; i < static_cast<uint16_t>(kHotbarSlots); ++i) {
        const int glfwKey = GLFW_KEY_1 + static_cast<int>(i);
        const bool pressed = glfwGetKey(m_Window, glfwKey) == GLFW_PRESS;
        if (!keyboardBlockedByUi && pressed && !m_WasHotbarSelectPressed[i]) {
            runtime.activeHotbarSlot = i;
        }
        m_WasHotbarSelectPressed[i] = pressed;
    }
}

void App::syncEquippedGunFromInventory(Runtime& runtime) {
    if (!runtime.inventoryUi || !runtime.inventoryUi->hasSnapshot()) {
        return;
    }

    if (runtime.activeHotbarSlot >= static_cast<uint16_t>(kHotbarSlots)) {
        runtime.activeHotbarSlot = 0;
    }

    const Slot& activeSlot = runtime.inventoryUi->slots()[runtime.activeHotbarSlot];
    if (Inventory::IsEmpty(activeSlot) || !Inventory::IsValidItemId(activeSlot.itemId)) {
        runtime.equippedGun = nullptr;
        return;
    }

    const std::optional<GunType> selectedGunType = GunTypeFromInventoryItemId(activeSlot.itemId);
    if (!selectedGunType.has_value()) {
        runtime.equippedGun = nullptr;
        return;
    }

    (void)equipGun(runtime, *selectedGunType);
}


void App::processWorldInteraction(Runtime& runtime) {
    const bool rightPressed = glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool rightClicked = rightPressed && !m_WasWorldInteractPressed;
    m_WasWorldInteractPressed = rightPressed;

    if (!runtime.localPlayerAlive || GameData::cursorEnabled || IsImGuiTextInputActive()) {
        return;
    }

    if (!rightClicked) {
        return;
    }

    Slot activeSlot{};
    bool hasActiveItem = false;
    ItemType activeItemType = ItemType::Other;
    std::optional<BlockID> activeBlockType = std::nullopt;
    if (
        runtime.inventoryUi &&
        runtime.inventoryUi->hasSnapshot() &&
        runtime.activeHotbarSlot < static_cast<uint16_t>(kHotbarSlots)
    ) {
        activeSlot = runtime.inventoryUi->slots()[runtime.activeHotbarSlot];
        hasActiveItem = !Inventory::IsEmpty(activeSlot) && Inventory::IsValidItemId(activeSlot.itemId);
        if (hasActiveItem) {
            activeItemType = Items::ItemDatabase[activeSlot.itemId].type;
            activeBlockType = BlockTypeFromInventoryItemId(activeSlot.itemId);
            if (activeItemType == ItemType::Block && !activeBlockType.has_value()) {
                activeBlockType = BlockID::Dirt;
            }
        }
    }

    if (hasActiveItem && activeItemType == ItemType::Gun) {
        return;
    }

    Ray ray(runtime.player->getCamera().position, runtime.player->getCamera().front);
    const RayResult hitResult = runtime.rayManager.rayHasBlockIntersectSingle(
        ray, *runtime.chunkManager, runtime.player->maxReach
    );
    if (!hitResult.hit) {
        return;
    }

    if (hasActiveItem && activeItemType == ItemType::Block && activeBlockType.has_value()) {
        const glm::ivec3 placePos = hitResult.adjacentAirBlockWorld;
        const BlockID previousBlock = runtime.chunkManager->getBlockGlobal(placePos.x, placePos.y, placePos.z);
        if (previousBlock != BlockID::Air) {
            return;
        }

        if (!runtime.clientNet.IsConnected()) {
            runtime.chunkManager->setBlockGlobal(placePos.x, placePos.y, placePos.z, *activeBlockType);
            MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, placePos);
            return;
        }

        BlockPlaceRequest request{};
        request.requestId = runtime.nextBlockPlaceRequestId++;
        request.edits.push_back(BlockPlaceEdit{
            placePos.x,
            placePos.y,
            placePos.z,
            static_cast<uint8_t>(*activeBlockType)
        });

        if (runtime.clientNet.SendBlockPlaceRequest(request)) {
            runtime.chunkManager->setBlockGlobal(placePos.x, placePos.y, placePos.z, *activeBlockType);
            MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, placePos);

            Runtime::PendingBlockPlaceRequest pending{};
            pending.createdAt = glfwGetTime();
            pending.affectedChunks = CollectChunkAndEdgeNeighbors(*runtime.chunkManager, placePos);
            pending.edits.push_back(Runtime::PendingBlockPlaceEdit{
                placePos,
                static_cast<uint8_t>(previousBlock),
                static_cast<uint8_t>(*activeBlockType)
            });
            runtime.pendingBlockPlaceRequests[request.requestId] = std::move(pending);
        }
        return;
    }

    if (!runtime.clientNet.IsConnected()) {
        runtime.chunkManager->playerBreakBlockAt(hitResult.hitBlockWorld);
        return;
    }

    const glm::ivec3 breakPos = hitResult.hitBlockWorld;
    const BlockID previousBlock = runtime.chunkManager->getBlockGlobal(breakPos.x, breakPos.y, breakPos.z);
    if (previousBlock == BlockID::Air) {
        return;
    }

    BlockBreakRequest request{};
    request.requestId = runtime.nextBlockBreakRequestId++;
    request.edits.push_back(BlockBreakEdit{
        breakPos.x,
        breakPos.y,
        breakPos.z
    });

    if (runtime.clientNet.SendBlockBreakRequest(request)) {
        runtime.chunkManager->playerBreakBlockAt(breakPos);

        Runtime::PendingBlockBreakRequest pending{};
        pending.createdAt = glfwGetTime();
        pending.affectedChunks = CollectChunkAndEdgeNeighbors(*runtime.chunkManager, breakPos);
        pending.edits.push_back(Runtime::PendingBlockBreakEdit{
            breakPos,
            static_cast<uint8_t>(previousBlock)
        });
        runtime.pendingBlockBreakRequests[request.requestId] = std::move(pending);
    }
}


void App::processShooting(Runtime& runtime) {
    if (!runtime.localPlayerAlive) {
        return;
    }

    if (GameData::cursorEnabled || m_UseDebugCamera) {
        return;
    }
    if (!runtime.clientNet.IsConnected()) {
        return;
    }
    if (!runtime.equippedGun) {
        return;
    }

    const bool triggerPressed = (glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    if (!triggerPressed) {
        return;
    }

    const double now = glfwGetTime();
    if ((now - runtime.lastShootSendTime) < runtime.shootSendInterval) {
        return;
    }

    const Camera& cam = runtime.player->getCamera();
    const float dirLenSq = glm::dot(cam.front, cam.front);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        return;
    }

    const glm::vec3 shootDir = glm::normalize(cam.front);
    const glm::vec3 shootPos = cam.position;
    const uint32_t shotId = runtime.nextClientShotId++;
    const uint32_t clientTick = runtime.hasAppliedServerTick ? runtime.lastAppliedServerTick : 0u;
    const uint32_t seed = shotId ^ (clientTick * 2654435761u);

    if (runtime.clientNet.SendShootRequest(
        shotId,
        clientTick,
        runtime.equippedGun->getWeaponId(),
        shootPos,
        shootDir,
        seed,
        0
    )) {
        runtime.lastShootSendTime = now;
    }
}


void App::processMovementNetworking(Runtime& runtime) {
    runtime.clientNet.Poll();
    if (runtime.inventoryUi) {
        runtime.inventoryUi->consumeNetwork(runtime.clientNet);
    }
    processHotbarSelection(runtime);
    syncEquippedGunFromInventory(runtime);

    const std::string& statusNow = runtime.clientNet.GetConnectionStatusText();
    if (statusNow != runtime.lastConnectionStatus) {
        std::cout << "[net] status: " << statusNow << "\n";
        runtime.lastConnectionStatus = statusNow;
        if (runtime.clientNet.IsConnected()) {
            runtime.usernamePromptError.clear();
        }
        else if (statusNow.find("username already taken") != std::string::npos) {
            runtime.usernamePromptError = "Username already taken. Enter a different username and retry.";
        }
    }

    const double now = glfwGetTime();
    const ClientNetwork::ConnectionState connState = runtime.clientNet.GetConnectionState();
    if (connState == ClientNetwork::ConnectionState::Disconnected) {
        if (runtime.clientNet.ShouldAutoReconnect() && now >= runtime.nextReconnectAttemptTime) {
            const bool started = beginConnectionAttempt(runtime);
            const double backoff = runtime.reconnectBackoffSeconds;
            runtime.nextReconnectAttemptTime = now + (started ? backoff : 2.0);
            runtime.reconnectBackoffSeconds = std::min(runtime.reconnectBackoffSeconds * 1.5, 8.0);
        }
    }
    else {
        runtime.reconnectBackoffSeconds = 1.0;
    }

    ShootResult shootResult{};
    while (runtime.clientNet.PopShootResult(shootResult)) {
        if (!shootResult.accepted) {
            std::cout << "[shoot] rejected shot id=" << shootResult.clientShotId << "\n";
            continue;
        }
        if (shootResult.didHit) {
            std::cout
                << "[shoot] hit id=" << shootResult.hitEntityId
                << " dmg=" << shootResult.damageApplied
                << " at=(" << shootResult.hitX << "," << shootResult.hitY << "," << shootResult.hitZ << ")\n";
        }
        else {
            std::cout
                << "[shoot] miss"
                << " at=(" << shootResult.hitX << "," << shootResult.hitY << "," << shootResult.hitZ << ")\n";
        }
    }

    ClientNetwork::KillFeedEvent killEvent{};
    while (runtime.clientNet.PopKillFeedEvent(killEvent)) {
        const std::string localName = runtime.clientNet.GetAssignedUsername();
        if (!localName.empty() && killEvent.victim == localName) {
            runtime.localDeathKiller = killEvent.killer;
        }

        Runtime::KillFeedEntry entry;
        entry.killer = std::move(killEvent.killer);
        entry.victim = std::move(killEvent.victim);
        entry.weaponId = killEvent.weaponId;
        entry.expiresAt = now + Runtime::KillFeedDurationSec;
        runtime.killFeedEntries.push_front(std::move(entry));
        while (runtime.killFeedEntries.size() > Runtime::MaxKillFeedEntries) {
            runtime.killFeedEntries.pop_back();
        }
    }

    ClientNetwork::ScoreboardSnapshot scoreboardSnapshot{};
    while (runtime.clientNet.PopScoreboardSnapshot(scoreboardSnapshot)) {
        runtime.matchRemainingSeconds = std::max(0, scoreboardSnapshot.remainingSeconds);
        runtime.matchStarted = scoreboardSnapshot.matchStarted;
        runtime.matchEnded = scoreboardSnapshot.matchEnded;
        runtime.matchWinner = std::move(scoreboardSnapshot.winner);
        runtime.scoreboardEntries = std::move(scoreboardSnapshot.entries);
    }

    WorldItemSnapshot worldItemSnapshot{};
    while (runtime.clientNet.PopWorldItemSnapshot(worldItemSnapshot)) {
        if (
            runtime.lastWorldItemSnapshotTick != 0 &&
            !IsNewerU32(worldItemSnapshot.serverTick, runtime.lastWorldItemSnapshotTick)
        ) {
            continue;
        }
        runtime.lastWorldItemSnapshotTick = worldItemSnapshot.serverTick;

        std::unordered_set<uint64_t> seenIds;
        seenIds.reserve(worldItemSnapshot.items.size());
        for (const WorldItemState& itemState : worldItemSnapshot.items) {
            seenIds.insert(itemState.id);
            Runtime::WorldItemVisual& item = runtime.worldItems[itemState.id];
            const glm::vec3 snapshotPos(itemState.px, itemState.py, itemState.pz);
            if (item.id == 0) {
                item.position = snapshotPos;
                item.targetPosition = snapshotPos;
            }
            item.id = itemState.id;
            item.itemId = itemState.itemId;
            item.quantity = itemState.quantity;
            item.targetPosition = snapshotPos;
            item.velocity = glm::vec3(itemState.vx, itemState.vy, itemState.vz);
        }

        for (auto it = runtime.worldItems.begin(); it != runtime.worldItems.end();) {
            if (seenIds.find(it->first) == seenIds.end()) {
                it = runtime.worldItems.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    bool hasNewestSelfSnapshot = false;
    uint32_t newestServerTick = 0;
    uint32_t newestAckedInputTick = 0;
    glm::vec3 newestServerPos(0.0f);
    glm::vec3 newestServerVel(0.0f);
    bool newestServerOnGround = false;
    bool newestServerFlyMode = false;
    bool newestServerAllowFlyMode = false;
    bool newestServerAlive = true;
    float newestServerHealth = 100.0f;
    float newestRespawnSeconds = 0.0f;
    bool newestServerJumpPressedLastTick = false;
    float newestServerTimeSinceGrounded = 0.0f;
    float newestServerJumpBufferTimer = 0.0f;

    std::vector<PlayerSnapshotFrame> queuedSnapshotFrames;
    queuedSnapshotFrames.reserve(8);
    PlayerSnapshotFrame snapshotFrame;
    while (runtime.clientNet.PopPlayerSnapshot(snapshotFrame)) {
        queuedSnapshotFrames.push_back(snapshotFrame);
    }

    for (const PlayerSnapshotFrame& frame : queuedSnapshotFrames) {
        if (runtime.hasReceivedSelfSnapshotTick &&
            !IsNewerU32(frame.serverTick, runtime.lastReceivedSelfSnapshotTick)) {
            continue;
        }

        runtime.snapshotInterpolator.PushFrame(frame);

        const PlayerSnapshot* localSnapshot = nullptr;
        for (const PlayerSnapshot& snapshot : frame.players) {
            if (snapshot.id == frame.selfPlayerId) {
                localSnapshot = &snapshot;
                break;
            }
        }
        if (localSnapshot == nullptr) {
            continue;
        }

        runtime.hasReceivedSelfSnapshotTick = true;
        runtime.lastReceivedSelfSnapshotTick = frame.serverTick;

        hasNewestSelfSnapshot = true;
        newestServerTick = frame.serverTick;
        newestAckedInputTick = frame.lastProcessedInputTick;
        newestServerPos = glm::vec3(localSnapshot->px, localSnapshot->py, localSnapshot->pz);
        newestServerVel = glm::vec3(localSnapshot->vx, localSnapshot->vy, localSnapshot->vz);
        newestServerOnGround = (localSnapshot->onGround != 0);
        newestServerFlyMode = (localSnapshot->flyMode != 0);
        newestServerAllowFlyMode = (localSnapshot->allowFlyMode != 0);
        newestServerAlive = (localSnapshot->isAlive != 0);
        newestServerHealth = std::max(0.0f, localSnapshot->health);
        newestRespawnSeconds = std::max(0.0f, localSnapshot->respawnSeconds);
        newestServerJumpPressedLastTick = (localSnapshot->jumpPressedLastTick != 0);
        newestServerTimeSinceGrounded = localSnapshot->timeSinceGrounded;
        newestServerJumpBufferTimer = localSnapshot->jumpBufferTimer;
    }

    double renderTime = 0.0;
    if (runtime.snapshotInterpolator.GetRenderTime(renderTime)) {
        std::vector<SnapshotInterpolator::InterpolatedPlayer> interpolated;
        runtime.snapshotInterpolator.BuildRemotePlayers(renderTime, interpolated);

        std::unordered_map<PlayerID, PlayerState> newestRemotePlayers;
        newestRemotePlayers.reserve(interpolated.size());
        for (const SnapshotInterpolator::InterpolatedPlayer& snapshot : interpolated) {
            PlayerState remoteState;
            remoteState.position = snapshot.position;
            remoteState.rotation = glm::angleAxis(
                glm::radians(ToModelYawDegrees(
                    NormalizeYawDegrees(snapshot.yawDegrees),
                    kDefaultPlayerModelYawInvert,
                    kDefaultPlayerModelYawOffsetDeg
                )),
                glm::vec3(0.0f, 1.0f, 0.0f)
            );
            remoteState.scale = glm::vec3(1.0f);
            remoteState.weaponId = snapshot.weaponId;
            newestRemotePlayers[snapshot.id] = remoteState;
        }
        runtime.player->setConnectedPlayers(newestRemotePlayers);
    }

    if (hasNewestSelfSnapshot) {
        ClientReconciler::ServerSnapshot snapshot{};
        snapshot.serverTick = newestServerTick;
        snapshot.ackedInputTick = newestAckedInputTick;
        snapshot.position = newestServerPos;
        snapshot.velocity = newestServerVel;
        snapshot.onGround = newestServerOnGround;
        snapshot.flyMode = newestServerFlyMode;
        snapshot.allowFlyMode = newestServerAllowFlyMode;
        snapshot.alive = newestServerAlive;
        snapshot.respawnSeconds = newestRespawnSeconds;
        snapshot.jumpPressedLastTick = newestServerJumpPressedLastTick;
        snapshot.timeSinceGrounded = newestServerTimeSinceGrounded;
        snapshot.jumpBufferTimer = newestServerJumpBufferTimer;
        runtime.reconciler.Apply(runtime, snapshot);
        runtime.localHealth = newestServerHealth;
        if (runtime.renderStateNeedsResync) {
            const Player::SimulationState state = runtime.player->captureSimulationState();
            runtime.renderPrevSimState = state;
            runtime.renderCurrSimState = state;
            runtime.localSimAccumulator = 0.0;
            runtime.renderStateNeedsResync = false;
        }
    }

    if (!runtime.clientNet.IsConnected()) {
        runtime.pendingInputs.clear();
        runtime.pendingBlockPlaceRequests.clear();
        runtime.nextBlockPlaceRequestId = 1;
        runtime.pendingBlockBreakRequests.clear();
        runtime.nextBlockBreakRequestId = 1;
        runtime.killFeedEntries.clear();
        runtime.matchRemainingSeconds = 600;
        runtime.matchStarted = false;
        runtime.matchEnded = false;
        runtime.matchWinner.clear();
        runtime.scoreboardEntries.clear();
        runtime.localPlayerAlive = true;
        runtime.localHealth = 100.0f;
        runtime.localRespawnSeconds = 0.0f;
        runtime.localDeathKiller.clear();
        runtime.wasRespawnClickDown = false;
        runtime.player->setFlyModeAllowed(false);
        runtime.player->clearConnectedPlayers();
        runtime.worldItems.clear();
        runtime.lastWorldItemSnapshotTick = 0;
        runtime.activeHotbarSlot = 0;
        runtime.snapshotInterpolator.Clear();
        runtime.hasAppliedServerTick = false;
        runtime.hasReceivedSelfSnapshotTick = false;
        runtime.inputTickCounter = 1;
        runtime.lastAckedInputTick = 0;
        runtime.lastInputSendTime = glfwGetTime();
        runtime.renderStateNeedsResync = false;
        runtime.hasRenderSimState = false;
        runtime.hasSmoothedPlayerCameraPos = false;
        m_ForceCursorEnabled = false;
        if (runtime.inventoryUi) {
            runtime.inventoryUi->reset();
        }
        return;
    }

    const bool respawnClickDown = (glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    if (!runtime.localPlayerAlive && runtime.localRespawnSeconds <= 0.0f) {
        if (respawnClickDown && !runtime.wasRespawnClickDown) {
            (void)runtime.clientNet.SendRespawnRequest();
        }
    }
    if (!runtime.localPlayerAlive && !runtime.pendingInputs.empty()) {
        runtime.pendingInputs.clear();
    }
    runtime.wasRespawnClickDown = respawnClickDown;

    // Send inputs at 60Hz with FRESH input data for each tick (not sampled once per frame)
    // This prevents "stair-step" movement when frame rate < 60Hz
    constexpr size_t kMaxInputSendsPerFrame = 4;
    size_t inputSendsThisFrame = 0;

    while (
        now - runtime.lastInputSendTime >= Runtime::InputSendInterval &&
        inputSendsThisFrame < kMaxInputSendsPerFrame
    ) {
        runtime.lastInputSendTime += Runtime::InputSendInterval;

        // Capture fresh input directly from keyboard (avoids 1-frame delay from m_networkInput)
        NetworkInputState input = runtime.player->captureCurrentInput(m_Window);
        if (!runtime.localPlayerAlive) {
            input.moveX = 0.0f;
            input.moveZ = 0.0f;
            input.flags = 0;
            input.flyMode = false;
        }
        
        PlayerInput packet;
        packet.inputTick = runtime.inputTickCounter++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = runtime.equippedGun ? runtime.equippedGun->getWeaponId() : kInventoryEmptyItemId;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (!runtime.clientNet.SendPlayerInput(packet)) {
            break;
        }

        if (runtime.localPlayerAlive) {
            Runtime::PendingInputEntry entry;
            entry.packet = packet;
            entry.deltaSeconds = Runtime::InputSendInterval;
            runtime.pendingInputs.push_back(entry);
            while (runtime.pendingInputs.size() > Runtime::MaxPendingInputs) {
                runtime.pendingInputs.pop_front();
            }

            size_t resentCopies = 0;
            for (
                auto pendingIt = runtime.pendingInputs.rbegin();
                pendingIt != runtime.pendingInputs.rend() &&
                resentCopies < Runtime::InputRedundancyCopies;
                ++pendingIt
            ) {
                const PlayerInput& resendPacket = pendingIt->packet;
                if (resendPacket.inputTick == packet.inputTick) {
                    continue;
                }
                if (IsAckedU32(resendPacket.inputTick, runtime.lastAckedInputTick)) {
                    continue;
                }
                if (!runtime.clientNet.SendPlayerInput(resendPacket)) {
                    break;
                }
                ++resentCopies;
            }
        }

        ++inputSendsThisFrame;
    }
    if (now - runtime.lastInputSendTime >= Runtime::InputSendInterval) {
        // Avoid unbounded backlog after long hitches.
        runtime.lastInputSendTime = now;
    }

    const glm::vec3 requestPos = runtime.player->getPosition();
    const glm::ivec3 worldPos(
        static_cast<int>(std::floor(requestPos.x)),
        static_cast<int>(std::floor(requestPos.y)),
        static_cast<int>(std::floor(requestPos.z))
    );
    const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
    const uint16_t viewDistance = static_cast<uint16_t>(std::max<int>(2, runtime.player->renderDistance));
    const bool centerChanged =
        !runtime.hasLastChunkRequestCenter ||
        centerChunk.x != runtime.lastChunkRequestCenter.x ||
        centerChunk.y != runtime.lastChunkRequestCenter.y ||
        centerChunk.z != runtime.lastChunkRequestCenter.z;
    const bool allowBurstRequest =
        !runtime.hasLastChunkRequestCenter ||
        (now - runtime.lastChunkRequestSendTime >= Runtime::ChunkRequestCenterChangeMinInterval);

    if (centerChanged && allowBurstRequest) {
        runtime.lastChunkRequestSendTime = now;
        runtime.lastChunkRequestCenter = centerChunk;
        runtime.hasLastChunkRequestCenter = true;
        (void)runtime.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
    else if (now - runtime.lastChunkRequestSendTime >= Runtime::ChunkRequestSendInterval) {
        runtime.lastChunkRequestSendTime = now;
        runtime.lastChunkRequestCenter = centerChunk;
        runtime.hasLastChunkRequestCenter = true;
        (void)runtime.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
}


void App::processChunkStreaming(Runtime& runtime, bool prioritizeMovement) {
    constexpr double kChunkResyncCooldownSec = 0.25;
    static std::unordered_map<glm::ivec3, double, IVec3Hash> s_chunkResyncCooldownUntil;

    const auto requestChunkResync = [&](const glm::ivec3& chunkPos, bool force) {
        const double nowSec = glfwGetTime();
        auto it = s_chunkResyncCooldownUntil.find(chunkPos);
        if (!force && it != s_chunkResyncCooldownUntil.end() && nowSec < it->second) {
            return;
        }
        s_chunkResyncCooldownUntil[chunkPos] = nowSec + kChunkResyncCooldownSec;
        if (!runtime.clientNet.SendChunkResyncRequest(chunkPos)) {
            std::cerr
                << "[chunk/resync] failed to request full chunk ("
                << chunkPos.x << "," << chunkPos.y << "," << chunkPos.z << ")\n";
        }
    };

    const int64_t chunkApplyBudgetUs = prioritizeMovement
        ? Runtime::ChunkApplyBudgetUsUnderInputPressure
        : Runtime::ChunkApplyBudgetUs;
    const auto chunkApplyStart = std::chrono::steady_clock::now();
    const auto withinChunkApplyBudget = [&]() -> bool {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - chunkApplyStart
        ).count();
        return elapsedUs < chunkApplyBudgetUs;
    };
    size_t chunkDataApplied = 0;

    ChunkData chunkData;
    while (
        chunkDataApplied < Runtime::MaxChunkDataApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkData(chunkData)
    ) {
        runtime.chunkManager->applyNetworkChunkData(chunkData);
        ++chunkDataApplied;
    }

    ChunkDelta chunkDelta;
    size_t chunkDeltaApplied = 0;
    while (
        chunkDeltaApplied < Runtime::MaxChunkDeltaApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkDelta(chunkDelta)
    ) {
        const NetworkChunkDeltaApplyResult deltaResult = runtime.chunkManager->applyNetworkChunkDelta(chunkDelta);
        if (
            deltaResult == NetworkChunkDeltaApplyResult::MissingBaseChunk ||
            deltaResult == NetworkChunkDeltaApplyResult::VersionGap
        ) {
            requestChunkResync(glm::ivec3(chunkDelta.chunkX, chunkDelta.chunkY, chunkDelta.chunkZ), false);
        }
        ++chunkDeltaApplied;
    }

    ChunkUnload chunkUnload;
    size_t chunkUnloadApplied = 0;
    while (
        chunkUnloadApplied < Runtime::MaxChunkUnloadApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkUnload(chunkUnload)
    ) {
        runtime.chunkManager->applyNetworkChunkUnload(chunkUnload);
        ++chunkUnloadApplied;
    }

    BlockPlaceResult blockPlaceResult;
    size_t blockPlaceResultsApplied = 0;
    while (
        blockPlaceResultsApplied < Runtime::MaxBlockPlaceResultsPerFrame &&
        runtime.clientNet.PopBlockPlaceResult(blockPlaceResult)
    ) {
        auto pendingIt = runtime.pendingBlockPlaceRequests.find(blockPlaceResult.requestId);
        if (blockPlaceResult.accepted == 0) {
            if (pendingIt != runtime.pendingBlockPlaceRequests.end()) {
                for (const Runtime::PendingBlockPlaceEdit& edit : pendingIt->second.edits) {
                    const BlockID predictedId = static_cast<BlockID>(edit.newBlockId);
                    const BlockID rollbackId = static_cast<BlockID>(edit.oldBlockId);
                    if (
                        runtime.chunkManager->getBlockGlobal(edit.worldPos.x, edit.worldPos.y, edit.worldPos.z) ==
                        predictedId
                    ) {
                        runtime.chunkManager->setBlockGlobal(
                            edit.worldPos.x,
                            edit.worldPos.y,
                            edit.worldPos.z,
                            rollbackId
                        );
                        MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, edit.worldPos);
                    }
                }
            }

            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToResync;
            if (pendingIt != runtime.pendingBlockPlaceRequests.end()) {
                for (const glm::ivec3& chunkPos : pendingIt->second.affectedChunks) {
                    chunksToResync.insert(chunkPos);
                }
            }
            else {
                for (const BlockPlaceChunkCoord& coord : blockPlaceResult.correctiveChunks) {
                    chunksToResync.insert(glm::ivec3(coord.chunkX, coord.chunkY, coord.chunkZ));
                }
            }

            for (const glm::ivec3& chunkPos : chunksToResync) {
                requestChunkResync(chunkPos, true);
            }
        }

        if (pendingIt != runtime.pendingBlockPlaceRequests.end()) {
            runtime.pendingBlockPlaceRequests.erase(pendingIt);
        }
        ++blockPlaceResultsApplied;
    }

    const double nowSec = glfwGetTime();
    for (auto it = runtime.pendingBlockPlaceRequests.begin(); it != runtime.pendingBlockPlaceRequests.end();) {
        if ((nowSec - it->second.createdAt) > 1.5) {
            for (const glm::ivec3& chunkPos : it->second.affectedChunks) {
                requestChunkResync(chunkPos, true);
            }
            it = runtime.pendingBlockPlaceRequests.erase(it);
        }
        else {
            ++it;
        }
    }

    BlockBreakResult blockBreakResult;
    size_t blockBreakResultsApplied = 0;
    while (
        blockBreakResultsApplied < Runtime::MaxBlockBreakResultsPerFrame &&
        runtime.clientNet.PopBlockBreakResult(blockBreakResult)
    ) {
        auto pendingIt = runtime.pendingBlockBreakRequests.find(blockBreakResult.requestId);
        if (blockBreakResult.accepted == 0) {
            if (pendingIt != runtime.pendingBlockBreakRequests.end()) {
                for (const Runtime::PendingBlockBreakEdit& edit : pendingIt->second.edits) {
                    if (runtime.chunkManager->getBlockGlobal(edit.worldPos.x, edit.worldPos.y, edit.worldPos.z) == BlockID::Air) {
                        runtime.chunkManager->setBlockGlobal(
                            edit.worldPos.x,
                            edit.worldPos.y,
                            edit.worldPos.z,
                            static_cast<BlockID>(edit.oldBlockId)
                        );
                        MarkChunkAndEdgeNeighborsDirty(*runtime.chunkManager, edit.worldPos);
                    }
                }
            }

            std::unordered_set<glm::ivec3, IVec3Hash, IVec3Eq> chunksToResync;
            if (pendingIt != runtime.pendingBlockBreakRequests.end()) {
                for (const glm::ivec3& chunkPos : pendingIt->second.affectedChunks) {
                    chunksToResync.insert(chunkPos);
                }
            }
            else {
                for (const BlockBreakChunkCoord& coord : blockBreakResult.correctiveChunks) {
                    chunksToResync.insert(glm::ivec3(coord.chunkX, coord.chunkY, coord.chunkZ));
                }
            }

            for (const glm::ivec3& chunkPos : chunksToResync) {
                requestChunkResync(chunkPos, true);
            }
        }

        if (pendingIt != runtime.pendingBlockBreakRequests.end()) {
            runtime.pendingBlockBreakRequests.erase(pendingIt);
        }
        ++blockBreakResultsApplied;
    }

    for (auto it = runtime.pendingBlockBreakRequests.begin(); it != runtime.pendingBlockBreakRequests.end();) {
        if ((nowSec - it->second.createdAt) > 1.5) {
            for (const glm::ivec3& chunkPos : it->second.affectedChunks) {
                requestChunkResync(chunkPos, true);
            }
            it = runtime.pendingBlockBreakRequests.erase(it);
        }
        else {
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

    const double now = glfwGetTime();
    if (kEnableChunkDiagnostics && now - runtime.lastChunkCoverageLogTime >= 1.0) {
        runtime.lastChunkCoverageLogTime = now;
        const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();

        const glm::vec3 pos = runtime.player->getPosition();
        const glm::ivec3 worldPos(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
        const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
        const int viewDistance = std::max<int>(2, runtime.player->renderDistance);
        const int64_t radius2 = static_cast<int64_t>(viewDistance) * static_cast<int64_t>(viewDistance);
        const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
        const int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

        const auto& chunks = runtime.chunkManager->getChunks();
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
                    if (!runtime.chunkManager->inBounds(cp)) continue;
                    ++desired;
                    if (chunks.find(cp) != chunks.end()) {
                        ++loaded;
                    }
                    else if (missingSamples.size() < 8) {
                        missingSamples.push_back(cp);
                    }
                }
            }
        }

        std::cerr
            << "[chunk/client] coverage center=("
            << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
            << " viewDist=" << viewDistance
            << " desired=" << desired
            << " loaded=" << loaded
            << " missing=" << (desired - loaded)
            << " queue(data/delta/unload)=("
            << queueDepths.chunkData << "/"
            << queueDepths.chunkDelta << "/"
            << queueDepths.chunkUnload << ")"
            << " applied(data/delta/unload)=("
            << chunkDataApplied << "/"
            << chunkDeltaApplied << "/"
            << chunkUnloadApplied << ")\n";

        if (!missingSamples.empty()) {
            std::cerr << "[chunk/client] missing samples:";
            for (const glm::ivec3& cp : missingSamples) {
                std::cerr << " (" << cp.x << "," << cp.y << "," << cp.z << ")";
            }
            std::cerr << "\n";
        }
    }
}


void App::processFrame(Runtime& runtime) {
    const auto perfFrameStart = std::chrono::steady_clock::now();
    const auto toMs = [](const auto& start, const auto& end) -> float {
        return static_cast<float>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
        ) * 0.001f;
    };

    const double frameNow = glfwGetTime();
    double frameDeltaSeconds = frameNow - GameData::lastFrame;
    if (!std::isfinite(frameDeltaSeconds) || frameDeltaSeconds < 0.0) {
        frameDeltaSeconds = 0.0;
    }
    // Clamp hitches so interpolation and prediction do not overreact to one bad frame.
    if (frameDeltaSeconds > 0.1) {
        frameDeltaSeconds = 0.1;
    }

    {
        static double s_lastFrameTimeLog = 0.0;
        constexpr double kFrameTimeLogThresholdMs = 12.5;
        constexpr double kFrameTimeLogCooldownSec = 0.5;
        const double frameMs = frameDeltaSeconds * 1000.0;
        if (frameMs >= kFrameTimeLogThresholdMs && (frameNow - s_lastFrameTimeLog) >= kFrameTimeLogCooldownSec) {
            const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();
            const double fps = frameMs > 0.0 ? (1000.0 / frameMs) : 0.0;
            std::cerr
                << "[frame] slow frameMs=" << frameMs
                << " fps=" << fps
                << " queue(data/delta/unload)=("
                << queueDepths.chunkData << "/"
                << queueDepths.chunkDelta << "/"
                << queueDepths.chunkUnload << ")\n";
            s_lastFrameTimeLog = frameNow;
        }
    }
    GameData::deltaTime = frameDeltaSeconds;
    GameData::lastFrame = frameNow;

    const auto perfInputStart = std::chrono::steady_clock::now();
    if (runtime.debugUi) {
        runtime.debugUi->beginFrame();
    }
    updateDebugCamera(runtime);
    updateToggleStates(runtime);

    if (!runtime.clientNet.IsConnected()) {
        GameData::cursorEnabled = true;
    }
    GameData::gameplayInputEnabled =
        runtime.clientNet.IsConnected() &&
        !m_ShowDebugUi &&
        !m_ForceCursorEnabled;

    runtime.inputCallbacks->processInput(m_Window);
    applyMouseInputModes();
    runtime.localSimAccumulator += GameData::deltaTime;
    const auto perfInputEnd = std::chrono::steady_clock::now();
    runtime.perfInputMs = toMs(perfInputStart, perfInputEnd);

    const double maxAccumulatedTime = Runtime::LocalPredictionStep * static_cast<double>(Runtime::MaxLocalPredictionStepsPerFrame);
    if (runtime.localSimAccumulator > maxAccumulatedTime) {
        runtime.localSimAccumulator = maxAccumulatedTime;
    }

    const auto perfNetworkStart = std::chrono::steady_clock::now();
    processMovementNetworking(runtime);
    const auto perfNetworkEnd = std::chrono::steady_clock::now();
    runtime.perfNetworkMs = toMs(perfNetworkStart, perfNetworkEnd);

    const auto perfPredictionStart = std::chrono::steady_clock::now();
    if (!runtime.hasRenderSimState) {
        const Player::SimulationState initialSimState = runtime.player->captureSimulationState();
        runtime.renderPrevSimState = initialSimState;
        runtime.renderCurrSimState = initialSimState;
        runtime.hasRenderSimState = true;
    }

    size_t localPredictionSteps = 0;
    if (runtime.localPlayerAlive) {
        while (
            runtime.localSimAccumulator >= Runtime::LocalPredictionStep &&
            localPredictionSteps < Runtime::MaxLocalPredictionStepsPerFrame
        ) {
            runtime.player->update(m_Window, Runtime::LocalPredictionStep);
            runtime.localSimAccumulator -= Runtime::LocalPredictionStep;
            runtime.renderPrevSimState = runtime.renderCurrSimState;
            runtime.renderCurrSimState = runtime.player->captureSimulationState();
            ++localPredictionSteps;
        }
        if (localPredictionSteps == 0) {
            // Keep input sampling responsive even on very high FPS frames.
            runtime.player->update(m_Window, 0.0);
        }
        if (
            localPredictionSteps == Runtime::MaxLocalPredictionStepsPerFrame &&
            runtime.localSimAccumulator >= Runtime::LocalPredictionStep
        ) {
            runtime.localSimAccumulator = std::fmod(runtime.localSimAccumulator, Runtime::LocalPredictionStep);
        }
    }
    else {
        runtime.localSimAccumulator = 0.0;
        runtime.renderStateNeedsResync = false;
        const Player::SimulationState frozenState = runtime.player->captureSimulationState();
        runtime.renderPrevSimState = frozenState;
        runtime.renderCurrSimState = frozenState;
    }
    const auto perfPredictionEnd = std::chrono::steady_clock::now();
    runtime.perfPredictionMs = toMs(perfPredictionStart, perfPredictionEnd);

    const auto perfGameplayStart = std::chrono::steady_clock::now();
    processWorldInteraction(runtime);
    runtime.player->updateRemotePlayers(static_cast<float>(GameData::deltaTime));
    processShooting(runtime);
    const auto perfGameplayEnd = std::chrono::steady_clock::now();
    runtime.perfGameplayMs = toMs(perfGameplayStart, perfGameplayEnd);

    const auto perfRenderStart = std::chrono::steady_clock::now();
    const Player::SimulationState simStateAfterPrediction = runtime.player->captureSimulationState();
    const glm::vec3 renderStateError = simStateAfterPrediction.position - runtime.renderCurrSimState.position;
    const float renderStateErrorSq = glm::dot(renderStateError, renderStateError);
    const float renderLatencyBlend = LatencyCorrectionBlend(runtime.clientNet);
    const float renderSnapDist = Runtime::BasicAuthReconcileTeleportDistance + 5.5f + (4.0f * renderLatencyBlend);
    const float renderStateSnapDistSq = renderSnapDist * renderSnapDist;
    if (renderStateErrorSq > renderStateSnapDistSq) {
        runtime.renderPrevSimState = simStateAfterPrediction;
        runtime.renderCurrSimState = simStateAfterPrediction;
        runtime.hasSmoothedPlayerCameraPos = false;
    }

    const Camera& latestCamera = runtime.player->getCamera();
    runtime.interpolatedPlayerCamera = latestCamera;

    const float simAlpha = std::clamp(
        static_cast<float>(runtime.localSimAccumulator / Runtime::LocalPredictionStep),
        0.0f,
        1.0f
    );
    const glm::vec3 interpolatedBodyPos = glm::mix(
        runtime.renderPrevSimState.position,
        runtime.renderCurrSimState.position,
        simAlpha
    );
    const glm::vec3 extrapolatedBodyPos =
        runtime.renderCurrSimState.position +
        runtime.renderCurrSimState.velocity * static_cast<float>(runtime.localSimAccumulator);
    // Keep the local camera on the same timeline as local prediction.
    // Extra render extrapolation tends to overshoot during rapid strafe-turns
    // and shows up as visible camera jitter.
    float renderExtrapolationBlend = 0.0f;
    glm::vec3 targetBodyPos = glm::mix(
        interpolatedBodyPos,
        extrapolatedBodyPos,
        renderExtrapolationBlend
    );
    const glm::vec3 renderLead = targetBodyPos - runtime.renderCurrSimState.position;
    const float renderLeadLenSq = glm::dot(renderLead, renderLead);
    const float renderLeadMaxSq = Runtime::RenderLeadMaxDistance * Runtime::RenderLeadMaxDistance;
    if (renderLeadLenSq > renderLeadMaxSq && renderLeadLenSq > 1e-8f) {
        const float renderLeadLen = std::sqrt(renderLeadLenSq);
        targetBodyPos = runtime.renderCurrSimState.position +
            renderLead * (Runtime::RenderLeadMaxDistance / renderLeadLen);
    }
    const float interpolatedStepOffset = glm::mix(
        runtime.renderPrevSimState.stepUpVisualOffset,
        runtime.renderCurrSimState.stepUpVisualOffset,
        simAlpha
    );
    const float eyeHeight = Shared::PlayerData::GetMovementSettings().eyeHeight;
    const glm::vec3 targetCameraPos =
        targetBodyPos + glm::vec3(0.0f, eyeHeight - interpolatedStepOffset, 0.0f);
    runtime.smoothedPlayerCameraPos = targetCameraPos;
    runtime.hasSmoothedPlayerCameraPos = true;
    runtime.interpolatedPlayerCamera.position = targetCameraPos;

    const float worldItemBlend = std::clamp(
        1.0f - std::exp(-14.0f * static_cast<float>(GameData::deltaTime)),
        0.0f,
        1.0f
    );
    for (auto& [_, item] : runtime.worldItems) {
        item.position = glm::mix(item.position, item.targetPosition, worldItemBlend);
    }

    const Camera& activeCamera = m_UseDebugCamera
        ? runtime.debugCamera
        : runtime.interpolatedPlayerCamera;

    RenderFrameParams frameParams{
        .chunkShader = *runtime.chunkShader,
        .debugShader = *runtime.dbgShader,
        .chunkManager = *runtime.chunkManager,
        .frustum = runtime.frustum,
        .player = *runtime.player,
        .activeCamera = activeCamera,
        .sky = runtime.sky,
        .toggleWireframe = m_ToggleWireframe,
        .toggleChunkBorders = m_ToggleChunkBorders,
        .toggleDebugFrustum = m_ToggleDebugFrustum,
        .chunkUniformsInitialized = &runtime.chunkUniformsInitialized
    };
    runtime.renderer.renderFrame(frameParams);
    renderWorldItems(runtime, activeCamera);
    renderRemotePlayerGuns(runtime, activeCamera);
    if (!m_UseDebugCamera && runtime.localPlayerAlive) {
        renderHeldGun(runtime, runtime.interpolatedPlayerCamera);
    }

    if (runtime.debugUi) {
        runtime.debugUi->drawCrosshair(!GameData::cursorEnabled && runtime.localPlayerAlive);
        if (runtime.debugUi->isVisible()) {
            const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();
            UiFrameData frameData;
            frameData.fps = (GameData::deltaTime > 1e-6) ? static_cast<float>(1.0 / GameData::deltaTime) : 0.0f;
            frameData.frameMs = static_cast<float>(GameData::deltaTime * 1000.0);
            frameData.playerPosition = runtime.player->getPosition();
            frameData.playerVelocity = runtime.player->getVelocity();
            frameData.flyMode = runtime.player->flyMode;
            frameData.onGround = runtime.player->isGrounded();
            frameData.renderDistance = runtime.player->renderDistance;
            frameData.remotePlayerCount = runtime.player->connectedPlayers.size();
            frameData.netConnected = runtime.clientNet.IsConnected();
            frameData.netStatus = runtime.clientNet.GetConnectionStatusText();
            frameData.serverTick = runtime.lastAppliedServerTick;
            frameData.ackedInputTick = runtime.lastAckedInputTick;
            frameData.pendingInputCount = runtime.pendingInputs.size();
            frameData.chunkDataQueueDepth = queueDepths.chunkData;
            frameData.chunkDeltaQueueDepth = queueDepths.chunkDelta;
            frameData.chunkUnloadQueueDepth = queueDepths.chunkUnload;
            frameData.backendName = runtime.renderer.getActiveBackendName();
            frameData.mdiUsable = runtime.renderer.isMDIUsable();
            frameData.perfFrameCpuMs = runtime.perfFrameCpuMs;
            frameData.perfInputMs = runtime.perfInputMs;
            frameData.perfNetworkMs = runtime.perfNetworkMs;
            frameData.perfPredictionMs = runtime.perfPredictionMs;
            frameData.perfGameplayMs = runtime.perfGameplayMs;
            frameData.perfRenderCpuMs = runtime.perfRenderCpuMs;
            frameData.perfPresentMs = runtime.perfPresentMs;
            frameData.perfChunkStreamingMs = runtime.perfChunkStreamingMs;

            UiMutableState mutableState;
            mutableState.useDebugCamera = &m_UseDebugCamera;
            mutableState.toggleWireframe = &m_ToggleWireframe;
            mutableState.toggleChunkBorders = &m_ToggleChunkBorders;
            mutableState.toggleDebugFrustum = &m_ToggleDebugFrustum;
            mutableState.renderDistance = &runtime.player->renderDistance;
            mutableState.cursorEnabled = &GameData::cursorEnabled;
            mutableState.rawMouseInputEnabled = &m_EnableRawMouseInput;
            mutableState.rawMouseInputSupported = glfwRawMouseMotionSupported();
            mutableState.gunViewOffset = &runtime.equippedGunViewOffset;
            mutableState.gunViewScale = &runtime.equippedGunViewScale;
            mutableState.gunViewEulerDeg = &runtime.equippedGunViewEulerDeg;

            runtime.debugUi->drawMainWindow(frameData, mutableState);
        }

        if (!runtime.debugUi->isVisible() && m_ShowDebugUi) {
            m_ShowDebugUi = false;
        }
        if (runtime.inventoryUi) {
            runtime.inventoryUi->draw(runtime.clientNet, runtime.clientNet.IsConnected());
            if (!runtime.inventoryUi->isVisible() && m_ShowInventoryUi) {
                m_ShowInventoryUi = false;
            }
        }

        GameData::cursorEnabled =
            m_ForceCursorEnabled || m_ShowDebugUi || m_ShowInventoryUi || !runtime.clientNet.IsConnected();
        applyMouseInputModes();

        drawConnectionPrompt(runtime);
        drawScoreboard(runtime);
        drawPingCounter(runtime);
        drawKillFeed(runtime);
        drawPlayerHud(runtime);
        drawDeathOverlay(runtime);
        runtime.debugUi->render();
    }

    static bool f11PressedLastFrame = false;

    bool f11PressedNow = glfwGetKey(m_Window, GLFW_KEY_F11) == GLFW_PRESS;

    if (f11PressedNow && !f11PressedLastFrame)
    {
        toggleFullscreen(m_Window);
    }

    f11PressedLastFrame = f11PressedNow;
    updateFPSCounter();
    const auto perfRenderEnd = std::chrono::steady_clock::now();
    runtime.perfRenderCpuMs = toMs(perfRenderStart, perfRenderEnd);

    const auto perfPresentStart = std::chrono::steady_clock::now();
    glfwSwapBuffers(m_Window);
    glfwPollEvents();
    const auto perfPresentEnd = std::chrono::steady_clock::now();
    runtime.perfPresentMs = toMs(perfPresentStart, perfPresentEnd);

    const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();
    const bool frameUnderPressure = GameData::deltaTime > (Runtime::LocalPredictionStep * 1.2);
    const bool chunkBacklog =
        queueDepths.chunkData > (Runtime::MaxChunkDataApplyPerFrame * 3) ||
        queueDepths.chunkDelta > (Runtime::MaxChunkDeltaApplyPerFrame * 3) ||
        queueDepths.chunkUnload > (Runtime::MaxChunkUnloadApplyPerFrame * 3);
    const bool prioritizeMovement = (localPredictionSteps > 1) || frameUnderPressure || chunkBacklog;
    const auto perfChunkStart = std::chrono::steady_clock::now();
    processChunkStreaming(runtime, prioritizeMovement);
    const auto perfChunkEnd = std::chrono::steady_clock::now();
    runtime.perfChunkStreamingMs = toMs(perfChunkStart, perfChunkEnd);

    const auto perfFrameEnd = std::chrono::steady_clock::now();
    runtime.perfFrameCpuMs = toMs(perfFrameStart, perfFrameEnd);
}
