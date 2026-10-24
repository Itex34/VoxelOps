#include "ShootValidation.hpp"

#include "../../engine/world/ChunkManager.hpp"
#include "../../engine/world/WorldRaycast.hpp"
#include "LagCompensation.hpp"

#include <algorithm>
#include <cmath>

namespace ShootValidation {

    bool IsDirectionValid(const ShootRequest &request, glm::vec3 &outNormalizedDir) {
        const glm::vec3 requestDir(request.dirX, request.dirY, request.dirZ);
        const float dirLenSq = glm::dot(requestDir, requestDir);
        if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
            return false;
        }
        outNormalizedDir = glm::normalize(requestDir);
        return true;
    }

    ShootGateResult
    RunShootGate(
        ShootGateState &state,
        const ShootRequest &request,
        float minShotIntervalSeconds,
        uint32_t shotServerTick,
        uint32_t serverTickRateHz
    ) {
        ShootGateResult gate{};
        if (state.hasLastShootClientShotId &&
            !LagCompensation::IsNewerU32(request.clientShotId, state.lastShootClientShotId)) {
            gate.rejectedReplay = true;
            return gate;
        }

        state.lastShootClientShotId = request.clientShotId;
        state.hasLastShootClientShotId = true;

        const uint32_t minimumIntervalTicks = static_cast<uint32_t>(std::ceil(
            std::max(0.0f, minShotIntervalSeconds) * static_cast<float>(serverTickRateHz)
        ));
        if (state.hasLastAcceptedShootTick) {
            const uint32_t elapsedTicks = shotServerTick - state.lastAcceptedShootTick;
            if (!LagCompensation::IsNewerU32(shotServerTick, state.lastAcceptedShootTick) ||
                elapsedTicks < minimumIntervalTicks) {
                gate.rejectedCooldown = true;
                return gate;
            }
        }

        state.lastAcceptedShootTick = shotServerTick;
        state.hasLastAcceptedShootTick = true;
        return gate;
    }

    ValidatedOrigin ComputeValidatedOrigin(
        const ChunkManager &chunkManager,
        const glm::vec3 &shooterEyePos,
        const glm::vec3 &requestPos,
        float originTolerance,
        float originOcclusionEpsilon
    ) {
        ValidatedOrigin out{};
        out.origin = shooterEyePos;
        out.requestedOriginAccepted = false;

        const glm::vec3 eyeToRequest = requestPos - shooterEyePos;
        const float eyeToRequestDist = glm::length(eyeToRequest);
        if (!std::isfinite(eyeToRequestDist) || eyeToRequestDist > originTolerance) {
            return out;
        }

        if (eyeToRequestDist <= 1e-4f) {
            out.origin = requestPos;
            out.requestedOriginAccepted = true;
            return out;
        }

        WorldRaycastResult rayResult = WorldRaycast::FindFirstSolidBlockHit(
            chunkManager,
            shooterEyePos,
            eyeToRequest,
            eyeToRequestDist
        );
        bool eyePathBlocked = rayResult.hit;

        out.requestedOriginAccepted =
            !eyePathBlocked || ((rayResult.distance + originOcclusionEpsilon) >= eyeToRequestDist);
        out.origin = out.requestedOriginAccepted ? requestPos : shooterEyePos;
        return out;
    }

} // namespace ShootValidation
