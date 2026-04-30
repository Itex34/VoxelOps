#include "ClientNetworkSystem.hpp"

#include "AppHelpers.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>

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

std::optional<GunType> GunTypeFromInventoryItemId(uint16_t itemId) {
    switch (itemId) {
    case static_cast<uint16_t>(ITEM_PISTOL):
        return GunType::Pistol;
    case static_cast<uint16_t>(ITEM_SNIPER):
        return GunType::Sniper;
    default:
        return std::nullopt;
    }
}
} // namespace

void ClientNetworkSystem::processHotbarSelection(Runtime &runtime) {
    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    for (uint16_t i = 0; i < static_cast<uint16_t>(kHotbarSlots); ++i) {
        const SDL_Scancode scancode =
            static_cast<SDL_Scancode>(SDL_SCANCODE_1 + static_cast<int>(i));
        const bool pressed = IsScancodeDown(scancode);
        if (!keyboardBlockedByUi && pressed && !m_wasHotbarSelectPressed[i]) {
            runtime.combat.activeHotbarSlot = i;
        }
        m_wasHotbarSelectPressed[i] = pressed;
    }
}

void ClientNetworkSystem::syncEquippedGunFromInventory(
    Runtime &runtime, const ClientNetworkSystemContext &ctx) {
    if (!runtime.gunRenderer) {
        runtime.combat.equippedGun = nullptr;
        return;
    }

    if (!runtime.inventoryUi || !runtime.inventoryUi->hasSnapshot()) {
        return;
    }

    if (runtime.combat.activeHotbarSlot >= static_cast<uint16_t>(kHotbarSlots)) {
        runtime.combat.activeHotbarSlot = 0;
    }

    const Slot &activeSlot = runtime.inventoryUi->slots()[runtime.combat.activeHotbarSlot];
    if (Inventory::IsEmpty(activeSlot) || !Inventory::IsValidItemId(activeSlot.itemId)) {
        runtime.combat.equippedGun = nullptr;
        return;
    }

    const std::optional<GunType> selectedGunType = GunTypeFromInventoryItemId(activeSlot.itemId);
    if (!selectedGunType.has_value()) {
        runtime.combat.equippedGun = nullptr;
        return;
    }

    if (ctx.equipGun) {
        (void)ctx.equipGun(runtime, *selectedGunType);
    }
}

void ClientNetworkSystem::update(Runtime &runtime, const ClientNetworkSystemContext &ctx) {
    runtime.clientNet.Poll();
    if (runtime.inventoryUi) {
        runtime.inventoryUi->consumeNetwork(runtime.clientNet);
    }
    processHotbarSelection(runtime);
    syncEquippedGunFromInventory(runtime, ctx);

    const std::string &statusNow = runtime.clientNet.GetConnectionStatusText();
    if (statusNow != runtime.connection.lastConnectionStatus) {
        std::cout << "[net] status: " << statusNow << "\n";
        runtime.connection.lastConnectionStatus = statusNow;
        if (runtime.clientNet.IsConnected()) {
            runtime.connection.usernamePromptError.clear();
        } else if (statusNow.find("username already taken") != std::string::npos) {
            runtime.connection.usernamePromptError =
                "Username already taken. Enter a different username and retry.";
        }
    }

    const double now = GetTimeSeconds();
    runtime.justRespawned = false;
    runtime.player->setTreatMissingCollisionAsSolid(now >= runtime.respawnMissingChunkGraceUntil);
    if (runtime.rbDiagActive && now >= runtime.rbDiagUntil) {
        runtime.rbDiagActive = false;
        std::cout << "[rbdiag/client] end window\n";
    }
    const ClientNetwork::ConnectionState connState = runtime.clientNet.GetConnectionState();
    if (connState == ClientNetwork::ConnectionState::Disconnected) {
        if (runtime.clientNet.ShouldAutoReconnect() &&
            now >= runtime.connection.nextReconnectAttemptTime) {
            const bool started = ctx.beginConnectionAttempt ? ctx.beginConnectionAttempt(runtime) : false;
            const double backoff = runtime.connection.reconnectBackoffSeconds;
            runtime.connection.nextReconnectAttemptTime = now + (started ? backoff : 2.0);
            runtime.connection.reconnectBackoffSeconds =
                std::min(runtime.connection.reconnectBackoffSeconds * 1.5, 8.0);
        }
    } else {
        runtime.connection.reconnectBackoffSeconds = 1.0;
    }

    ShootResult shootResult{};
    while (runtime.clientNet.PopShootResult(shootResult)) {
        if (!shootResult.accepted) {
            std::cout << "[shoot] rejected shot id=" << shootResult.clientShotId << "\n";
            continue;
        }
        if (shootResult.didHit) {
            std::cout << "[shoot] hit id=" << shootResult.hitEntityId
                      << " dmg=" << shootResult.damageApplied << " at=(" << shootResult.hitX << ","
                      << shootResult.hitY << "," << shootResult.hitZ << ")\n";
        } else {
            std::cout << "[shoot] miss"
                      << " at=(" << shootResult.hitX << "," << shootResult.hitY << ","
                      << shootResult.hitZ << ")\n";
        }
    }

    ClientNetwork::KillFeedEvent killEvent{};
    while (runtime.clientNet.PopKillFeedEvent(killEvent)) {
        const std::string localName = runtime.clientNet.GetAssignedUsername();
        if (!localName.empty() && killEvent.victim == localName) {
            runtime.combat.localDeathKiller = killEvent.killer;
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
        if (runtime.lastWorldItemSnapshotTick != 0 &&
            !IsNewerU32(worldItemSnapshot.serverTick, runtime.lastWorldItemSnapshotTick)) {
            continue;
        }
        runtime.lastWorldItemSnapshotTick = worldItemSnapshot.serverTick;

        std::unordered_set<uint64_t> seenIds;
        seenIds.reserve(worldItemSnapshot.items.size());
        for (const WorldItemState &itemState : worldItemSnapshot.items) {
            seenIds.insert(itemState.id);
            Runtime::WorldItemVisual &item = runtime.worldItems[itemState.id];
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
            } else {
                ++it;
            }
        }
    }

    bool hasNewestSelfSnapshot = false;
    uint64_t newestSelfPlayerId = 0;
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

    for (const PlayerSnapshotFrame &frame : queuedSnapshotFrames) {
        if (runtime.hasReceivedSelfSnapshotTick &&
            !IsNewerU32(frame.serverTick, runtime.lastReceivedSelfSnapshotTick)) {
            continue;
        }

        runtime.snapshotInterpolator.PushFrame(frame);

        const PlayerSnapshot *localSnapshot = nullptr;
        for (const PlayerSnapshot &snapshot : frame.players) {
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
        runtime.localPlayerId = frame.selfPlayerId;
        runtime.hasLocalPlayerId = true;

        hasNewestSelfSnapshot = true;
        newestSelfPlayerId = frame.selfPlayerId;
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
        for (const SnapshotInterpolator::InterpolatedPlayer &snapshot : interpolated) {
            if (runtime.hasLocalPlayerId && snapshot.id == runtime.localPlayerId) {
                continue;
            }
            if (hasNewestSelfSnapshot && snapshot.id == newestSelfPlayerId) {
                continue;
            }
            PlayerState remoteState;
            remoteState.position = snapshot.position;
            remoteState.rotation =
                glm::angleAxis(glm::radians(ToModelYawDegrees(
                                   NormalizeYawDegrees(snapshot.yawDegrees),
                                   kDefaultPlayerModelYawInvert, kDefaultPlayerModelYawOffsetDeg)),
                               glm::vec3(0.0f, 1.0f, 0.0f));
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
        runtime.combat.localHealth = newestServerHealth;
        if (runtime.justRespawned) {
            runtime.respawnMissingChunkGraceUntil = now + Runtime::RespawnMissingChunkGraceSeconds;
            runtime.player->setTreatMissingCollisionAsSolid(false);
            // Respawn teleports can shift chunk center instantly.
            runtime.hasLastChunkRequestCenter = false;
            runtime.lastChunkRequestSendTime = 0.0;
            runtime.rbDiagActive = true;
            runtime.rbDiagUntil = now + Runtime::RespawnDiagDurationSeconds;
            runtime.rbDiagNextHeartbeatAt = now;
            std::cout << "[rbdiag/client] respawn start"
                      << " serverTick=" << runtime.lastAppliedServerTick
                      << " ackedInputTick=" << runtime.lastAckedInputTick << " pos=("
                      << newestServerPos.x << "," << newestServerPos.y << "," << newestServerPos.z
                      << ")"
                      << " vel=(" << newestServerVel.x << "," << newestServerVel.y << ","
                      << newestServerVel.z << ")"
                      << "\n";
        }
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
        runtime.combat.localPlayerAlive = true;
        runtime.combat.localHealth = 100.0f;
        runtime.combat.localRespawnSeconds = 0.0f;
        runtime.combat.localDeathKiller.clear();
        runtime.combat.wasRespawnClickDown = false;
        runtime.player->setFlyModeAllowed(false);
        runtime.player->clearConnectedPlayers();
        runtime.worldItems.clear();
        runtime.lastWorldItemSnapshotTick = 0;
        runtime.combat.activeHotbarSlot = 0;
        runtime.snapshotInterpolator.Clear();
        runtime.hasLocalPlayerId = false;
        runtime.localPlayerId = 0;
        runtime.hasAppliedServerTick = false;
        runtime.hasReceivedSelfSnapshotTick = false;
        runtime.inputTickCounter = 1;
        runtime.lastAckedInputTick = 0;
        runtime.lastInputSendTime = GetTimeSeconds();
        runtime.lastChunkRequestSendTime = 0.0;
        runtime.hasLastChunkRequestCenter = false;
        runtime.renderStateNeedsResync = false;
        runtime.justRespawned = false;
        runtime.respawnMissingChunkGraceUntil = 0.0;
        runtime.player->setTreatMissingCollisionAsSolid(true);
        runtime.rbDiagActive = false;
        runtime.rbDiagUntil = 0.0;
        runtime.rbDiagNextHeartbeatAt = 0.0;
        runtime.hasRenderSimState = false;
        runtime.hasSmoothedPlayerCameraPos = false;
        if (ctx.forceCursorEnabled) {
            *ctx.forceCursorEnabled = false;
        }
        if (runtime.inventoryUi) {
            runtime.inventoryUi->reset();
        }
        return;
    }

    if (runtime.rbDiagActive && now >= runtime.rbDiagNextHeartbeatAt) {
        runtime.rbDiagNextHeartbeatAt = now + 1.0;
        const Player::SimulationState simState = runtime.player->captureSimulationState();
        const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();
        const int32_t unackedTicks =
            static_cast<int32_t>(runtime.inputTickCounter - runtime.lastAckedInputTick);
        std::cout << "[rbdiag/client] heartbeat"
                  << " serverTick=" << runtime.lastAppliedServerTick
                  << " ackedInputTick=" << runtime.lastAckedInputTick
                  << " inputTickCounter=" << runtime.inputTickCounter
                  << " unackedTicks=" << unackedTicks
                  << " pendingInputs=" << runtime.pendingInputs.size()
                  << " alive=" << (runtime.combat.localPlayerAlive ? 1 : 0)
                  << " respawnSeconds=" << runtime.combat.localRespawnSeconds
                  << " health=" << runtime.combat.localHealth << " pos=(" << simState.position.x
                  << "," << simState.position.y << "," << simState.position.z << ")"
                  << " vel=(" << simState.velocity.x << "," << simState.velocity.y << ","
                  << simState.velocity.z << ")"
                  << " onGround=" << (simState.onGround ? 1 : 0) << " missingChunkSolid="
                  << (runtime.player->isTreatMissingCollisionAsSolid() ? 1 : 0)
                  << " queue(data/delta/unload)=(" << queueDepths.chunkData << "/"
                  << queueDepths.chunkDelta << "/" << queueDepths.chunkUnload << ")"
                  << "\n";
    }

    const bool respawnClickDown = IsMouseButtonDown(SDL_BUTTON_LEFT);
    if (!runtime.combat.localPlayerAlive && runtime.combat.localRespawnSeconds <= 0.0f) {
        if (respawnClickDown && !runtime.combat.wasRespawnClickDown) {
            (void)runtime.clientNet.SendRespawnRequest();
        }
    }
    if (!runtime.combat.localPlayerAlive && !runtime.pendingInputs.empty()) {
        runtime.pendingInputs.clear();
    }
    runtime.combat.wasRespawnClickDown = respawnClickDown;

    // Send inputs at 60Hz with FRESH input data for each tick (not sampled once per frame)
    // This prevents "stair-step" movement when frame rate < 60Hz
    constexpr size_t kMaxInputSendsPerFrame = 4;
    size_t inputSendsThisFrame = 0;

    while (now - runtime.lastInputSendTime >= Runtime::InputSendInterval &&
           inputSendsThisFrame < kMaxInputSendsPerFrame) {
        runtime.lastInputSendTime += Runtime::InputSendInterval;

        // Capture fresh input directly from keyboard (avoids 1-frame delay from m_networkInput)
        NetworkInputState input = runtime.player->captureCurrentInput(ctx.window);
        if (!runtime.combat.localPlayerAlive) {
            input.moveX = 0.0f;
            input.moveZ = 0.0f;
            input.flags = 0;
            input.flyMode = false;
        }

        PlayerInput packet;
        packet.inputTick = runtime.inputTickCounter++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = runtime.combat.equippedGun
                              ? runtime.combat.equippedGun->getWeaponId()
                              : kInventoryEmptyItemId;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (!runtime.clientNet.SendPlayerInput(packet)) {
            break;
        }

        if (runtime.combat.localPlayerAlive) {
            Runtime::PendingInputEntry entry;
            entry.packet = packet;
            entry.deltaSeconds = Runtime::InputSendInterval;
            runtime.pendingInputs.push_back(entry);
            while (runtime.pendingInputs.size() > Runtime::MaxPendingInputs) {
                runtime.pendingInputs.pop_front();
            }

            size_t resentCopies = 0;
            for (auto pendingIt = runtime.pendingInputs.rbegin();
                 pendingIt != runtime.pendingInputs.rend() &&
                 resentCopies < Runtime::InputRedundancyCopies;
                 ++pendingIt) {
                const PlayerInput &resendPacket = pendingIt->packet;
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
    const glm::ivec3 worldPos(static_cast<int>(std::floor(requestPos.x)),
                              static_cast<int>(std::floor(requestPos.y)),
                              static_cast<int>(std::floor(requestPos.z)));
    const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
    const uint16_t viewDistance = static_cast<uint16_t>(std::max<int>(2, runtime.player->renderDistance));
    const bool centerChanged = !runtime.hasLastChunkRequestCenter ||
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
    } else if (now - runtime.lastChunkRequestSendTime >= Runtime::ChunkRequestSendInterval) {
        runtime.lastChunkRequestSendTime = now;
        runtime.lastChunkRequestCenter = centerChunk;
        runtime.hasLastChunkRequestCenter = true;
        (void)runtime.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
}
