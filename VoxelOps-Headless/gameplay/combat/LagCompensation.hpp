#pragma once

#include "../../player/ServerPlayer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace LagCompensation {

constexpr uint32_t kServerTickRateHz = 60u;
constexpr float kShootLagCompensationWindowSeconds = 0.300f;
constexpr uint32_t kShootLagCompensationMaxTicks = static_cast<uint32_t>(
    kShootLagCompensationWindowSeconds * static_cast<float>(kServerTickRateHz) + 0.5f);
constexpr size_t kShootLagCompensationMaxFrames =
    static_cast<size_t>(kShootLagCompensationMaxTicks + 4u);

struct LagCompPlayerPose {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;
    float height = 2.56f;
    float radius = 0.3f;
};

struct LagCompFrame {
    uint32_t serverTick = 0;
    std::unordered_map<PlayerID, LagCompPlayerPose> players;
};

bool IsNewerU32(uint32_t a, uint32_t b);

void RecordFrame(std::deque<LagCompFrame> &frames,
                 uint32_t serverTick,
                 const std::vector<ServerPlayerCombatSnapshot> &players);

const LagCompFrame *GetFrameForTick(const std::deque<LagCompFrame> &frames,
                                    uint32_t currentServerTick,
                                    uint32_t clientTick);

} // namespace LagCompensation
