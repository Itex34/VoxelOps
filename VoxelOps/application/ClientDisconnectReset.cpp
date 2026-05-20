#include "ClientDisconnectReset.hpp"

#include "AppHelpers.hpp"

using namespace AppHelpers;

void ClientDisconnectReset::apply(Runtime &runtime, bool *forceCursorEnabled) const {
    runtime.prediction.pendingInputs.clear();
    runtime.world.pendingBlockPlaceRequests.clear();
    runtime.world.nextBlockPlaceRequestId = 1;
    runtime.world.pendingBlockBreakRequests.clear();
    runtime.world.nextBlockBreakRequestId = 1;
    runtime.ui.killFeedEntries.clear();
    runtime.ui.matchRemainingSeconds = 600;
    runtime.ui.matchStarted = false;
    runtime.ui.matchEnded = false;
    runtime.ui.matchWinner.clear();
    runtime.ui.scoreboardEntries.clear();
    runtime.ui.activeView = UiView::MainMenu;
    runtime.ui.wantsCursor = true;
    runtime.combat.localPlayerAlive = true;
    runtime.combat.localHealth = 100.0f;
    runtime.combat.localRespawnSeconds = 0.0f;
    runtime.combat.localDeathKiller.clear();
    runtime.combat.wasRespawnClickDown = false;
    runtime.gameplay.player->setFlyModeAllowed(false);
    runtime.gameplay.player->clearConnectedPlayers();
    runtime.world.worldItems.clear();
    runtime.world.lastWorldItemSnapshotTick = 0;
    runtime.combat.grapple = RuntimeCombatState::GrappleRuntimeState{};
    runtime.combat.activeHotbarSlot = 0;
    runtime.network.snapshotInterpolator.Clear();
    runtime.prediction.hasLocalPlayerId = false;
    runtime.prediction.localPlayerId = 0;
    runtime.prediction.hasAppliedServerTick = false;
    runtime.prediction.hasReceivedSelfSnapshotTick = false;
    runtime.prediction.inputTickCounter = 1;
    runtime.prediction.lastAckedInputTick = 0;
    runtime.prediction.lastInputSendTime = GetTimeSeconds();
    runtime.world.lastChunkRequestSendTime = 0.0;
    runtime.world.hasLastChunkRequestCenter = false;
    runtime.world.renderStateNeedsResync = false;
    runtime.world.justRespawned = false;
    runtime.world.respawnMissingChunkGraceUntil = 0.0;
    runtime.gameplay.player->setTreatMissingCollisionAsSolid(true);
    runtime.world.rbDiagActive = false;
    runtime.world.rbDiagUntil = 0.0;
    runtime.world.rbDiagNextHeartbeatAt = 0.0;
    runtime.prediction.hasRenderSimState = false;
    runtime.prediction.hasSmoothedPlayerCameraPos = false;
    if (forceCursorEnabled != nullptr) {
        *forceCursorEnabled = false;
    }
    if (runtime.ui.inventoryUi) {
        runtime.ui.inventoryUi->reset();
    }
}
