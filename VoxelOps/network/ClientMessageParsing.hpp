#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ClientNetwork.hpp"

namespace ClientMessages {

bool ParseIntToken(std::string_view token, int &out);
bool ParseUint32Token(std::string_view token, uint32_t &out);
bool TryParseKillFeedMessage(const std::string &message, ClientNetwork::KillFeedEvent &out);
bool TryParseScoreboardMessage(const std::string &message, ClientNetwork::ScoreboardSnapshot &out);

} // namespace ClientMessages
