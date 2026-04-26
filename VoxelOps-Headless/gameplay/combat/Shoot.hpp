#pragma once

#include "Damage.hpp"
#include "LagCompensation.hpp"
#include "../../physics/HitDetection.hpp"
#include "../../player/ServerPlayer.hpp"

#include "../../../Shared/network/Packets.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <string>

class ChunkManager;

namespace Shoot {

struct ShootSessionSnapshot {
    std::string username;
    PlayerID playerId = 0;
    bool registered = false;
};

struct ShootContext {
    ShootRequest request{};
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    ShootSessionSnapshot session{};
    uint32_t serverTick = 0;
    const LagCompensation::LagCompFrame *lagFrame = nullptr;
    ServerPlayer shooter{};
    glm::vec3 rayOrigin{0.0f};
    glm::vec3 rayDir{0.0f};
    bool requestedOriginAccepted = false;
    ShootResult result{};
};

struct ShootOutcome {
    bool accepted = false;
    bool hit = false;
    PlayerID hitPlayerId = 0;
    HitRegion hitRegion = HitRegion::Unknown;
    glm::vec3 hitPoint{0.0f};
    float damageApplied = 0.0f;
    float healthAfter = 0.0f;
    bool killed = false;
};

ShootContext BuildContext(HSteamNetConnection connection,
                          const ShootRequest &request,
                          uint32_t currentServerTick);

ShootOutcome ResolveHit(const ShootContext &ctx,
                        const HitDetection::HitDetectionResult &hit,
                        float blockOcclusionEpsilon,
                        PlayerManager &playerManager);

bool FinalizeContext(ShootContext &ctx,
                     const ServerPlayer &shooter,
                     const LagCompensation::LagCompFrame *lagFrame,
                     const ChunkManager &chunkManager,
                     float eyeHeight,
                     float originTolerance,
                     float originOcclusionEpsilon);

void ApplyOutcomeToResult(ShootContext &ctx, const ShootOutcome &outcome);

} // namespace Shoot
