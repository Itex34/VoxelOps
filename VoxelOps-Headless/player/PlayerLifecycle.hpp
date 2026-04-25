#pragma once

#include "ServerPlayer.hpp"

#include <glm/vec3.hpp>

#include <list>
#include <memory>
#include <unordered_map>

namespace PlayerLifecycle {
void onPlayerConnect(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                     std::list<PlayerID> &playersOrder,
                     PlayerID id,
                     std::shared_ptr<ConnectionHandle> conn,
                     const glm::vec3 &spawnPos,
                     Clock::time_point now);

bool removePlayer(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                  std::list<PlayerID> &playersOrder,
                  PlayerID id);

bool touchHeartbeat(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                    PlayerID id,
                    Clock::time_point now);

bool requestRespawn(std::unordered_map<PlayerID, ServerPlayer> &playersById,
                    PlayerID id,
                    Clock::time_point now);
} // namespace PlayerLifecycle
