#pragma once

#include "../../../Shared/network/Packets.hpp"

#include <glm/vec3.hpp>

class ChunkManager;

namespace ShootValidation {

    struct ShootGateState {
        uint32_t lastAcceptedShootTick = 0;
        bool hasLastAcceptedShootTick = false;
        uint32_t lastShootClientShotId = 0;
        bool hasLastShootClientShotId = false;
    };

    struct ShootGateResult {
        bool rejectedReplay = false;
        bool rejectedCooldown = false;
    };

    struct ValidatedOrigin {
        glm::vec3 origin{0.0f};
        bool requestedOriginAccepted = false;
    };

    bool IsDirectionValid(const ShootRequest &request, glm::vec3 &outNormalizedDir);

    ShootGateResult
    RunShootGate(
        ShootGateState &state,
        const ShootRequest &request,
        float minShotIntervalSeconds,
        uint32_t shotServerTick,
        uint32_t serverTickRateHz
    );

    ValidatedOrigin ComputeValidatedOrigin(
        const ChunkManager &chunkManager,
        const glm::vec3 &shooterEyePos,
        const glm::vec3 &requestPos,
        float originTolerance,
        float originOcclusionEpsilon
    );

} // namespace ShootValidation
