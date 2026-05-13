#include "ClientSession.hpp"

#include "AppHelpers.hpp"

#include "../../Shared/items/Items.hpp"
#include "../../Shared/player/Inventory.hpp"

#include <SDL3/SDL.h>

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

void ClientSession::processHotbarSelection(Runtime &runtime) {
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

void ClientSession::syncEquippedGunFromInventory(
    Runtime &runtime, const ClientSessionContext &ctx
) {
    if (!runtime.render.gunRenderer) {
        runtime.combat.equippedGun = nullptr;
        return;
    }

    if (!runtime.ui.inventoryUi || !runtime.ui.inventoryUi->hasSnapshot()) {
        return;
    }

    if (runtime.combat.activeHotbarSlot >= static_cast<uint16_t>(kHotbarSlots)) {
        runtime.combat.activeHotbarSlot = 0;
    }

    const Slot &activeSlot = runtime.ui.inventoryUi->slots()[runtime.combat.activeHotbarSlot];
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

void ClientSession::update(
    Runtime &runtime, const ClientSessionContext &ctx, const ClientInputIntent *inputIntent
) {
    runtime.network.clientNet.Poll();
    if (runtime.ui.inventoryUi) {
        runtime.ui.inventoryUi->consumeNetwork(runtime.network.clientNet);
    }
    processHotbarSelection(runtime);
    syncEquippedGunFromInventory(runtime, ctx);

    const std::string &statusNow = runtime.network.clientNet.GetConnectionStatusText();
    if (statusNow != runtime.app.connection.lastConnectionStatus) {
        std::cout << "[net] status: " << statusNow << "\n";
        runtime.app.connection.lastConnectionStatus = statusNow;
        if (runtime.network.clientNet.IsConnected()) {
            runtime.app.connection.usernamePromptError.clear();
        } else if (statusNow.find("username already taken") != std::string::npos) {
            runtime.app.connection.usernamePromptError =
                "Username already taken. Enter a different username and retry.";
        }
    }

    const double now = GetTimeSeconds();
    runtime.world.justRespawned = false;
    runtime.gameplay.player->setTreatMissingCollisionAsSolid(now >= runtime.world.respawnMissingChunkGraceUntil);
    if (runtime.world.rbDiagActive && now >= runtime.world.rbDiagUntil) {
        runtime.world.rbDiagActive = false;
        std::cout << "[rbdiag/client] end window\n";
    }
    const ClientNetwork::ConnectionState connState = runtime.network.clientNet.GetConnectionState();
    if (connState == ClientNetwork::ConnectionState::Disconnected) {
        if (runtime.network.clientNet.ShouldAutoReconnect() &&
            now >= runtime.app.connection.nextReconnectAttemptTime) {
            const bool started =
                ctx.beginConnectionAttempt ? ctx.beginConnectionAttempt(runtime) : false;
            const double backoff = runtime.app.connection.reconnectBackoffSeconds;
            runtime.app.connection.nextReconnectAttemptTime = now + (started ? backoff : 2.0);
            runtime.app.connection.reconnectBackoffSeconds =
                std::min(runtime.app.connection.reconnectBackoffSeconds * 1.5, 8.0);
        }
    } else {
        runtime.app.connection.reconnectBackoffSeconds = 1.0;
    }

    ShootResult shootResult{};
    while (runtime.network.clientNet.PopShootResult(shootResult)) {
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
    while (runtime.network.clientNet.PopKillFeedEvent(killEvent)) {
        const std::string localName = runtime.network.clientNet.GetAssignedUsername();
        if (!localName.empty() && killEvent.victim == localName) {
            runtime.combat.localDeathKiller = killEvent.killer;
        }

        RuntimeUiState::KillFeedEntry entry;
        entry.killer = std::move(killEvent.killer);
        entry.victim = std::move(killEvent.victim);
        entry.weaponId = killEvent.weaponId;
        entry.expiresAt = now + RuntimeUiState::KillFeedDurationSec;
        runtime.ui.killFeedEntries.push_front(std::move(entry));
        while (runtime.ui.killFeedEntries.size() > RuntimeUiState::MaxKillFeedEntries) {
            runtime.ui.killFeedEntries.pop_back();
        }
    }

    ClientNetwork::ScoreboardSnapshot scoreboardSnapshot{};
    while (runtime.network.clientNet.PopScoreboardSnapshot(scoreboardSnapshot)) {
        runtime.ui.matchRemainingSeconds = std::max(0, scoreboardSnapshot.remainingSeconds);
        runtime.ui.matchStarted = scoreboardSnapshot.matchStarted;
        runtime.ui.matchEnded = scoreboardSnapshot.matchEnded;
        runtime.ui.matchWinner = std::move(scoreboardSnapshot.winner);
        runtime.ui.scoreboardEntries = std::move(scoreboardSnapshot.entries);
    }

    WorldItemSnapshot worldItemSnapshot{};
    while (runtime.network.clientNet.PopWorldItemSnapshot(worldItemSnapshot)) {
        if (runtime.world.lastWorldItemSnapshotTick != 0 &&
            !IsNewerU32(worldItemSnapshot.serverTick, runtime.world.lastWorldItemSnapshotTick)) {
            continue;
        }
        runtime.world.lastWorldItemSnapshotTick = worldItemSnapshot.serverTick;

        std::unordered_set<uint64_t> seenIds;
        seenIds.reserve(worldItemSnapshot.items.size());
        for (const WorldItemState &itemState : worldItemSnapshot.items) {
            seenIds.insert(itemState.id);
            RuntimeWorldState::WorldItemVisual &item = runtime.world.worldItems[itemState.id];
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

        for (auto it = runtime.world.worldItems.begin(); it != runtime.world.worldItems.end();) {
            if (seenIds.find(it->first) == seenIds.end()) {
                it = runtime.world.worldItems.erase(it);
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
    while (runtime.network.clientNet.PopPlayerSnapshot(snapshotFrame)) {
        queuedSnapshotFrames.push_back(snapshotFrame);
    }

    for (const PlayerSnapshotFrame &frame : queuedSnapshotFrames) {
        if (runtime.prediction.hasReceivedSelfSnapshotTick &&
            !IsNewerU32(frame.serverTick, runtime.prediction.lastReceivedSelfSnapshotTick)) {
            continue;
        }

        runtime.network.snapshotInterpolator.PushFrame(frame);

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

        runtime.prediction.hasReceivedSelfSnapshotTick = true;
        runtime.prediction.lastReceivedSelfSnapshotTick = frame.serverTick;
        runtime.prediction.localPlayerId = frame.selfPlayerId;
        runtime.prediction.hasLocalPlayerId = true;

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
    if (runtime.network.snapshotInterpolator.GetRenderTime(renderTime)) {
        std::vector<SnapshotInterpolator::InterpolatedPlayer> interpolated;
        runtime.network.snapshotInterpolator.BuildRemotePlayers(renderTime, interpolated);

        std::unordered_map<PlayerID, PlayerState> newestRemotePlayers;
        newestRemotePlayers.reserve(interpolated.size());
        for (const SnapshotInterpolator::InterpolatedPlayer &snapshot : interpolated) {
            if (runtime.prediction.hasLocalPlayerId && snapshot.id == runtime.prediction.localPlayerId) {
                continue;
            }
            if (hasNewestSelfSnapshot && snapshot.id == newestSelfPlayerId) {
                continue;
            }
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
        runtime.gameplay.player->setConnectedPlayers(newestRemotePlayers);
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
        runtime.network.reconciler.Apply(runtime, snapshot);
        runtime.combat.localHealth = newestServerHealth;
        if (runtime.world.justRespawned) {
            runtime.world.respawnMissingChunkGraceUntil = now + RuntimeWorldState::RespawnMissingChunkGraceSeconds;
            runtime.gameplay.player->setTreatMissingCollisionAsSolid(false);
            // Respawn teleports can shift chunk center instantly.
            runtime.world.hasLastChunkRequestCenter = false;
            runtime.world.lastChunkRequestSendTime = 0.0;
            runtime.world.rbDiagActive = true;
            runtime.world.rbDiagUntil = now + RuntimeWorldState::RespawnDiagDurationSeconds;
            runtime.world.rbDiagNextHeartbeatAt = now;
            std::cout << "[rbdiag/client] respawn start"
                      << " serverTick=" << runtime.prediction.lastAppliedServerTick
                      << " ackedInputTick=" << runtime.prediction.lastAckedInputTick << " pos=("
                      << newestServerPos.x << "," << newestServerPos.y << "," << newestServerPos.z
                      << ")"
                      << " vel=(" << newestServerVel.x << "," << newestServerVel.y << ","
                      << newestServerVel.z << ")"
                      << "\n";
        }
        if (runtime.world.renderStateNeedsResync) {
            const Player::SimulationState state = runtime.gameplay.player->captureSimulationState();
            const Player::PresentationState presentationState =
                runtime.gameplay.player->capturePresentationState();
            runtime.prediction.renderPrevSimState = state;
            runtime.prediction.renderCurrSimState = state;
            runtime.prediction.renderPrevPresentationState = presentationState;
            runtime.prediction.renderCurrPresentationState = presentationState;
            runtime.prediction.localSimAccumulator = 0.0;
            runtime.world.renderStateNeedsResync = false;
        }
    }

    if (!runtime.network.clientNet.IsConnected()) {
        m_disconnectReset.apply(runtime, ctx.forceCursorEnabled);
        return;
    }

    if (runtime.world.rbDiagActive && now >= runtime.world.rbDiagNextHeartbeatAt) {
        runtime.world.rbDiagNextHeartbeatAt = now + 1.0;
        const Player::SimulationState simState = runtime.gameplay.player->captureSimulationState();
        const ClientNetwork::ChunkQueueDepths queueDepths = runtime.network.clientNet.GetChunkQueueDepths();
        const int32_t unackedTicks =
            static_cast<int32_t>(runtime.prediction.inputTickCounter - runtime.prediction.lastAckedInputTick);
        std::cout << "[rbdiag/client] heartbeat"
                  << " serverTick=" << runtime.prediction.lastAppliedServerTick
                  << " ackedInputTick=" << runtime.prediction.lastAckedInputTick
                  << " inputTickCounter=" << runtime.prediction.inputTickCounter
                  << " unackedTicks=" << unackedTicks
                  << " pendingInputs=" << runtime.prediction.pendingInputs.size()
                  << " alive=" << (runtime.combat.localPlayerAlive ? 1 : 0)
                  << " respawnSeconds=" << runtime.combat.localRespawnSeconds
                  << " health=" << runtime.combat.localHealth << " pos=(" << simState.position.x
                  << "," << simState.position.y << "," << simState.position.z << ")"
                  << " vel=(" << simState.velocity.x << "," << simState.velocity.y << ","
                  << simState.velocity.z << ")"
                  << " onGround=" << (simState.onGround ? 1 : 0) << " missingChunkSolid="
                  << (runtime.gameplay.player->isTreatMissingCollisionAsSolid() ? 1 : 0)
                  << " queue(data/delta/unload)=(" << queueDepths.chunkData << "/"
                  << queueDepths.chunkDelta << "/" << queueDepths.chunkUnload << ")"
                  << "\n";
    }

    const bool respawnClickDown = IsMouseButtonDown(SDL_BUTTON_LEFT);
    if (!runtime.combat.localPlayerAlive && runtime.combat.localRespawnSeconds <= 0.0f) {
        if (respawnClickDown && !runtime.combat.wasRespawnClickDown) {
            (void)runtime.network.clientNet.SendRespawnRequest();
        }
    }
    if (!runtime.combat.localPlayerAlive && !runtime.prediction.pendingInputs.empty()) {
        runtime.prediction.pendingInputs.clear();
    }
    runtime.combat.wasRespawnClickDown = respawnClickDown;

    constexpr size_t kMaxInputSendsPerFrame = 4;
    size_t inputSendsThisFrame = 0;

    while (now - runtime.prediction.lastInputSendTime >= RuntimePredictionState::InputSendInterval &&
           inputSendsThisFrame < kMaxInputSendsPerFrame) {
        runtime.prediction.lastInputSendTime += RuntimePredictionState::InputSendInterval;

        NetworkInputState input =
            (inputIntent != nullptr) ? inputIntent->networkInput
                                     : runtime.gameplay.player->getNetworkInputState();
        if (!runtime.combat.localPlayerAlive) {
            input.moveX = 0.0f;
            input.moveZ = 0.0f;
            input.flags = 0;
            input.flyMode = false;
        }

        PlayerInput packet;
        packet.inputTick = runtime.prediction.inputTickCounter++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = runtime.combat.equippedGun ? runtime.combat.equippedGun->getWeaponId()
                                                     : kInventoryEmptyItemId;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (!runtime.network.clientNet.SendPlayerInput(packet)) {
            break;
        }

        if (runtime.combat.localPlayerAlive) {
            RuntimePredictionState::PendingInputEntry entry;
            entry.packet = packet;
            entry.deltaSeconds = RuntimePredictionState::InputSendInterval;
            runtime.prediction.pendingInputs.push_back(entry);
            while (runtime.prediction.pendingInputs.size() > RuntimePredictionState::MaxPendingInputs) {
                runtime.prediction.pendingInputs.pop_front();
            }

            size_t resentCopies = 0;
            for (auto pendingIt = runtime.prediction.pendingInputs.rbegin();
                 pendingIt != runtime.prediction.pendingInputs.rend() &&
                 resentCopies < RuntimePredictionState::InputRedundancyCopies;
                 ++pendingIt) {
                const PlayerInput &resendPacket = pendingIt->packet;
                if (resendPacket.inputTick == packet.inputTick) {
                    continue;
                }
                if (IsAckedU32(resendPacket.inputTick, runtime.prediction.lastAckedInputTick)) {
                    continue;
                }
                if (!runtime.network.clientNet.SendPlayerInput(resendPacket)) {
                    break;
                }
                ++resentCopies;
            }
        }

        ++inputSendsThisFrame;
    }
    if (now - runtime.prediction.lastInputSendTime >= RuntimePredictionState::InputSendInterval) {
        // Avoid unbounded backlog after long hitches.
        runtime.prediction.lastInputSendTime = now;
    }

    const glm::vec3 requestPos = runtime.gameplay.player->getPosition();
    const glm::ivec3 worldPos(
        static_cast<int>(std::floor(requestPos.x)),
        static_cast<int>(std::floor(requestPos.y)),
        static_cast<int>(std::floor(requestPos.z))
    );
    const glm::ivec3 centerChunk = runtime.gameplay.chunkManager->worldToChunkPos(worldPos);
    const uint16_t viewDistance =
        static_cast<uint16_t>(std::max<int>(2, runtime.gameplay.player->renderDistance));
    const bool centerChanged = !runtime.world.hasLastChunkRequestCenter ||
                               centerChunk.x != runtime.world.lastChunkRequestCenter.x ||
                               centerChunk.y != runtime.world.lastChunkRequestCenter.y ||
                               centerChunk.z != runtime.world.lastChunkRequestCenter.z;
    const bool allowBurstRequest =
        !runtime.world.hasLastChunkRequestCenter ||
        (now - runtime.world.lastChunkRequestSendTime >= RuntimeWorldState::ChunkRequestCenterChangeMinInterval);

    if (centerChanged && allowBurstRequest) {
        runtime.world.lastChunkRequestSendTime = now;
        runtime.world.lastChunkRequestCenter = centerChunk;
        runtime.world.hasLastChunkRequestCenter = true;
        (void)runtime.network.clientNet.SendChunkRequest(centerChunk, viewDistance);
    } else if (now - runtime.world.lastChunkRequestSendTime >= RuntimeWorldState::ChunkRequestSendInterval) {
        runtime.world.lastChunkRequestSendTime = now;
        runtime.world.lastChunkRequestCenter = centerChunk;
        runtime.world.hasLastChunkRequestCenter = true;
        (void)runtime.network.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
}









