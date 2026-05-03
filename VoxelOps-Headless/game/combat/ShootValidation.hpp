#pragma once

#include "../../../Shared/network/Packets.hpp"

#include <glm/vec3.hpp>

#include <chrono>

class ChunkManager;

namespace ShootValidation {

    struct ShootGateState {
        std::chrono::steady_clock::time_point lastAcceptedShootTime =
            std::chrono::steady_clock::time_point::min();
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
    RunShootGate(ShootGateState &state, const ShootRequest &request, float minShotIntervalSeconds);

    ValidatedOrigin ComputeValidatedOrigin(
        const ChunkManager &chunkManager,
        const glm::vec3 &shooterEyePos,
        const glm::vec3 &requestPos,
        float originTolerance,
        float originOcclusionEpsilon
    );

} // namespace ShootValidation
