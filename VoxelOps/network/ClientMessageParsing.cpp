#include "ClientMessageParsing.hpp"

#include <limits>
#include <utility>
#include <vector>

namespace ClientMessages {

bool ParseIntToken(std::string_view token, int &out) {
    if (token.empty()) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (token[0] == '-') {
        negative = true;
        i = 1;
    }
    if (i >= token.size()) {
        return false;
    }
    int value = 0;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const int digit = c - '0';
        if (value > ((std::numeric_limits<int>::max)() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = negative ? -value : value;
    return true;
}

bool ParseUint32Token(std::string_view token, uint32_t &out) {
    if (token.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint64_t>(c - '0');
        if (value > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
            return false;
        }
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool TryParseKillFeedMessage(const std::string &message, ClientNetwork::KillFeedEvent &out) {
    constexpr std::string_view kPrefix = "KILLFEED|";
    if (message.size() <= kPrefix.size() || message.rfind(kPrefix.data(), 0) != 0) {
        return false;
    }

    const size_t killerStart = kPrefix.size();
    const size_t killerSep = message.find('|', killerStart);
    if (killerSep == std::string::npos || killerSep == killerStart) {
        return false;
    }
    const size_t victimStart = killerSep + 1;
    const size_t victimSep = message.find('|', victimStart);
    if (victimSep == std::string::npos || victimSep == victimStart) {
        return false;
    }
    const size_t weaponStart = victimSep + 1;
    if (weaponStart >= message.size()) {
        return false;
    }

    uint16_t weaponId = 0;
    try {
        const unsigned long weaponRaw = std::stoul(message.substr(weaponStart));
        if (weaponRaw > 0xFFFFu) {
            return false;
        }
        weaponId = static_cast<uint16_t>(weaponRaw);
    } catch (...) {
        return false;
    }

    out.killer = message.substr(killerStart, killerSep - killerStart);
    out.victim = message.substr(victimStart, victimSep - victimStart);
    out.weaponId = weaponId;
    return !out.killer.empty() && !out.victim.empty();
}

bool TryParseScoreboardMessage(const std::string &message, ClientNetwork::ScoreboardSnapshot &out) {
    constexpr std::string_view kPrefix = "SCOREBOARD|";
    if (message.size() <= kPrefix.size() || message.rfind(kPrefix.data(), 0) != 0) {
        return false;
    }

    std::vector<std::string_view> fields;
    const std::string_view msgView(message);
    size_t start = kPrefix.size();
    while (start <= msgView.size()) {
        const size_t sep = msgView.find('|', start);
        if (sep == std::string_view::npos) {
            fields.push_back(msgView.substr(start));
            break;
        }
        fields.push_back(msgView.substr(start, sep - start));
        start = sep + 1;
    }

    if (fields.size() < 4) {
        return false;
    }

    int remaining = 0;
    int endedRaw = 0;
    int startedRaw = 1;
    int expectedCount = 0;
    if (!ParseIntToken(fields[0], remaining) || remaining < 0) {
        return false;
    }
    if (!ParseIntToken(fields[1], endedRaw) || (endedRaw != 0 && endedRaw != 1)) {
        return false;
    }

    size_t winnerIndex = 2;
    size_t countIndex = 3;
    size_t entriesStartIndex = 4;
    if (fields.size() >= 5) {
        // New format: remaining|ended|started|winner|count|...
        int parsedStarted = 1;
        if (ParseIntToken(fields[2], parsedStarted) &&
            (parsedStarted == 0 || parsedStarted == 1)) {
            startedRaw = parsedStarted;
            winnerIndex = 3;
            countIndex = 4;
            entriesStartIndex = 5;
        }
    }

    if (!ParseIntToken(fields[countIndex], expectedCount) || expectedCount < 0) {
        return false;
    }

    const size_t expectedFields = entriesStartIndex + static_cast<size_t>(expectedCount);
    if (fields.size() != expectedFields) {
        return false;
    }

    ClientNetwork::ScoreboardSnapshot snapshot;
    snapshot.remainingSeconds = remaining;
    snapshot.matchEnded = (endedRaw != 0);
    snapshot.matchStarted = (startedRaw != 0);
    snapshot.winner =
        (fields[winnerIndex] == "-") ? std::string() : std::string(fields[winnerIndex]);
    snapshot.entries.reserve(static_cast<size_t>(expectedCount));

    for (size_t i = 0; i < static_cast<size_t>(expectedCount); ++i) {
        const std::string_view entryField = fields[entriesStartIndex + i];
        const size_t c1 = entryField.find(',');
        if (c1 == std::string_view::npos) {
            return false;
        }
        const size_t c2 = entryField.find(',', c1 + 1);
        if (c2 == std::string_view::npos) {
            return false;
        }
        const size_t c3 = entryField.find(',', c2 + 1);
        if (c3 == std::string_view::npos) {
            return false;
        }
        if (entryField.find(',', c3 + 1) != std::string_view::npos) {
            return false;
        }

        const std::string_view name = entryField.substr(0, c1);
        const std::string_view killsTok = entryField.substr(c1 + 1, c2 - c1 - 1);
        const std::string_view deathsTok = entryField.substr(c2 + 1, c3 - c2 - 1);
        const std::string_view pingTok = entryField.substr(c3 + 1);

        if (name.empty()) {
            return false;
        }

        uint32_t kills = 0;
        uint32_t deaths = 0;
        int pingMs = -1;
        if (!ParseUint32Token(killsTok, kills) || !ParseUint32Token(deathsTok, deaths)) {
            return false;
        }
        if (!ParseIntToken(pingTok, pingMs)) {
            return false;
        }

        ClientNetwork::ScoreboardEntry entry;
        entry.username = std::string(name);
        entry.kills = kills;
        entry.deaths = deaths;
        entry.pingMs = pingMs;
        snapshot.entries.push_back(std::move(entry));
    }

    out = std::move(snapshot);
    return true;
}

} // namespace ClientMessages
