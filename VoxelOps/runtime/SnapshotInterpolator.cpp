#include "SnapshotInterpolator.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kSnapshotTickSeconds = 1.0 / 60.0;

float NormalizeYawDegrees(float yawDegrees) {
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f) y -= 360.0f;
    if (y < -180.0f) y += 360.0f;
    return y;
}

float LerpYawDegrees(float from, float to, float t) {
    const float delta = NormalizeYawDegrees(to - from);
    return from + delta * t;
}
}

void SnapshotInterpolator::PushFrame(const PlayerSnapshotFrame& frame)
{
    const double frameServerTimeSeconds =
        static_cast<double>(frame.serverTick) * kSnapshotTickSeconds;

    if (!m_hasLatestServerTimeSeconds || frameServerTimeSeconds > m_latestServerTimeSeconds) {
        m_latestServerTimeSeconds = frameServerTimeSeconds;
        m_hasLatestServerTimeSeconds = true;
    }

    for (const PlayerSnapshot& snapshot : frame.players) {
        if (snapshot.id == frame.selfPlayerId) {
            continue;
        }
        if (snapshot.isAlive == 0) {
            m_history.erase(snapshot.id);
            continue;
        }

        RemoteSnapshot remote{};
        remote.serverTick = frame.serverTick;
        remote.position = glm::vec3(snapshot.px, snapshot.py, snapshot.pz);
        remote.velocity = glm::vec3(snapshot.vx, snapshot.vy, snapshot.vz);
        remote.yawDegrees = snapshot.yaw;
        remote.weaponId = snapshot.weaponId;
        remote.serverTimeSeconds = frameServerTimeSeconds;
        AddSnapshot(snapshot.id, remote);
    }
}

bool SnapshotInterpolator::GetRenderTime(double& outRenderTime) const
{
    if (!m_hasLatestServerTimeSeconds) {
        return false;
    }
    outRenderTime = m_latestServerTimeSeconds - m_interpolationDelaySeconds;
    return true;
}

bool SnapshotInterpolator::BuildRemotePlayers(double renderTime, std::vector<InterpolatedPlayer>& outPlayers) const
{
    outPlayers.clear();
    if (!m_hasLatestServerTimeSeconds) {
        return false;
    }

    for (const auto& [id, history] : m_history) {
        if (history.empty()) {
            continue;
        }

        const RemoteSnapshot* bestOlder = nullptr;
        const RemoteSnapshot* bestNewer = nullptr;
        double olderTimeDiff = 0.0;
        double newerTimeDiff = 0.0;

        for (const RemoteSnapshot& snap : history) {
            if (snap.serverTimeSeconds <= renderTime) {
                const double diff = renderTime - snap.serverTimeSeconds;
                if (!bestOlder || diff < olderTimeDiff) {
                    bestOlder = &snap;
                    olderTimeDiff = diff;
                }
            }
            else {
                const double diff = snap.serverTimeSeconds - renderTime;
                if (!bestNewer || diff < newerTimeDiff) {
                    bestNewer = &snap;
                    newerTimeDiff = diff;
                }
            }
        }

        if (!bestOlder && !bestNewer) {
            continue;
        }

        InterpolatedPlayer out{};
        out.id = id;

        if (bestOlder && bestNewer && (olderTimeDiff + newerTimeDiff) > 0.001) {
            const float alpha = static_cast<float>(olderTimeDiff / (olderTimeDiff + newerTimeDiff));
            out.position = glm::mix(bestOlder->position, bestNewer->position, alpha);
            out.yawDegrees = LerpYawDegrees(bestOlder->yawDegrees, bestNewer->yawDegrees, alpha);
            out.weaponId = (alpha >= 0.5f) ? bestNewer->weaponId : bestOlder->weaponId;
        }
        else if (bestOlder) {
            const double timeSinceSnapshot = renderTime - bestOlder->serverTimeSeconds;
            out.position = bestOlder->position + bestOlder->velocity * static_cast<float>(timeSinceSnapshot);
            out.yawDegrees = bestOlder->yawDegrees;
            out.weaponId = bestOlder->weaponId;
        }
        else {
            // Only have newer snapshot (rare case)
            const RemoteSnapshot* pick = bestNewer;
            out.position = pick->position;
            out.yawDegrees = pick->yawDegrees;
            out.weaponId = pick->weaponId;
        }

        outPlayers.push_back(out);
    }

    return !outPlayers.empty();
}

void SnapshotInterpolator::Clear()
{
    m_history.clear();
    m_hasLatestServerTimeSeconds = false;
    m_latestServerTimeSeconds = 0.0;
}

void SnapshotInterpolator::AddSnapshot(PlayerID id, const RemoteSnapshot& snapshot)
{
    auto& history = m_history[id];
    for (RemoteSnapshot& existing : history) {
        if (existing.serverTick == snapshot.serverTick) {
            existing = snapshot;
            return;
        }
    }

    history.push_back(snapshot);
    if (history.size() > m_maxSnapshotsPerPlayer) {
        history.pop_front();
    }
}
