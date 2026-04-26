#include "LagCompensation.hpp"

namespace LagCompensation {

bool IsNewerU32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

void RecordFrame(std::deque<LagCompFrame> &frames,
                 uint32_t serverTick,
                 const std::vector<ServerPlayerCombatSnapshot> &players) {
    LagCompFrame frame{};
    frame.serverTick = serverTick;
    frame.players.reserve(players.size());
    for (const ServerPlayerCombatSnapshot &player : players) {
        LagCompPlayerPose pose{};
        pose.position = player.position;
        pose.yaw = player.yaw;
        pose.height = player.height;
        pose.radius = player.radius;
        frame.players.emplace(player.id, pose);
    }

    frames.push_back(std::move(frame));
    while (frames.size() > kShootLagCompensationMaxFrames) {
        frames.pop_front();
    }
    while (!frames.empty()) {
        const uint32_t oldestTick = frames.front().serverTick;
        if (!IsNewerU32(serverTick, oldestTick)) {
            break;
        }
        if ((serverTick - oldestTick) <= kShootLagCompensationMaxTicks) {
            break;
        }
        frames.pop_front();
    }
}

const LagCompFrame *GetFrameForTick(const std::deque<LagCompFrame> &frames,
                                    uint32_t currentServerTick,
                                    uint32_t clientTick) {
    uint32_t lagCompTargetTick = currentServerTick;
    if (clientTick <= currentServerTick &&
        (currentServerTick - clientTick) <= kShootLagCompensationMaxTicks) {
        lagCompTargetTick = clientTick;
    }
    if (frames.empty()) {
        return nullptr;
    }
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        if (!IsNewerU32(it->serverTick, lagCompTargetTick)) {
            return &(*it);
        }
    }
    return nullptr;
}

} // namespace LagCompensation
