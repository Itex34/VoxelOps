#pragma once

#include "../player/Player.hpp"

#include "../../Shared/network/Packets.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <deque>

struct RuntimePredictionState {
    struct PendingInputEntry {
        PlayerInput packet{};
        double deltaSeconds = 0.0;
    };

    static constexpr size_t MaxPendingInputs = 256;
    static constexpr double LocalPredictionStep = 1.0 / 60.0;
    static constexpr size_t MaxLocalPredictionStepsPerFrame = 8;
    static constexpr float BasicAuthReconcileDeadzone = 0.08f;
    static constexpr float BasicAuthReconcileTeleportDistance = 2.0f;
    static constexpr float RenderLeadMaxDistance = 0.40f;
    static constexpr size_t InputRedundancyCopies = 0;

    uint32_t inputTickCounter = 1;
    uint64_t localPlayerId = 0;
    bool hasLocalPlayerId = false;
    uint32_t lastAckedInputTick = 0;
    uint32_t lastAppliedServerTick = 0;
    bool hasAppliedServerTick = false;
    uint32_t lastReceivedSelfSnapshotTick = 0;
    bool hasReceivedSelfSnapshotTick = false;
    double localSimAccumulator = 0.0;
    std::deque<PendingInputEntry> pendingInputs;
    double lastInputSendTime = 0.0;

    Player::SimulationState renderPrevSimState{};
    Player::SimulationState renderCurrSimState{};
    Player::PresentationState renderPrevPresentationState{};
    Player::PresentationState renderCurrPresentationState{};
    bool hasRenderSimState = false;
    glm::vec3 smoothedPlayerCameraPos{0.0f};
    bool hasSmoothedPlayerCameraPos = false;
};
