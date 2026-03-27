#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "../player/Player.hpp"
#include "../../Shared/network/Packets.hpp"

class SnapshotInterpolator {
public:
    struct InterpolatedPlayer {
        PlayerID id = 0;
        glm::vec3 position{ 0.0f };
        float yawDegrees = 0.0f;
        uint16_t weaponId = 0;
    };

    void PushFrame(const PlayerSnapshotFrame& frame);
    bool GetRenderTime(double& outRenderTime) const;
    bool BuildRemotePlayers(double renderTime, std::vector<InterpolatedPlayer>& outPlayers) const;
    void Clear();

    void SetInterpolationDelaySeconds(double seconds) { m_interpolationDelaySeconds = seconds; }
    double GetInterpolationDelaySeconds() const { return m_interpolationDelaySeconds; }

private:
    struct RemoteSnapshot {
        uint32_t serverTick = 0;
        glm::vec3 position{ 0.0f };
        glm::vec3 velocity{ 0.0f };
        float yawDegrees = 0.0f;
        uint16_t weaponId = 0;
        double serverTimeSeconds = 0.0;
    };

    std::unordered_map<PlayerID, std::deque<RemoteSnapshot>> m_history;
    double m_latestServerTimeSeconds = 0.0;
    bool m_hasLatestServerTimeSeconds = false;
    double m_interpolationDelaySeconds = 0.100;
    size_t m_maxSnapshotsPerPlayer = 32;

    void AddSnapshot(PlayerID id, const RemoteSnapshot& snapshot);
};
