#include "ClientNetwork.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ws2tcpip.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace {
uint32_t fnv1a32(const uint8_t* data, size_t size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint32_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}

constexpr size_t kMaxChunkDataQueueDepth = 128;
constexpr size_t kMaxChunkDeltaQueueDepth = 512;
constexpr size_t kMaxChunkUnloadQueueDepth = 256;
constexpr size_t kMaxKillFeedQueueDepth = 64;
constexpr size_t kMaxScoreboardQueueDepth = 16;
constexpr size_t kMaxMessagesPerPoll = 128;
constexpr int64_t kMessagePollBudgetUs = 2000;
constexpr const char* kClientIdentityFileName = "client_identity.txt";
constexpr bool kEnableClientNetProfiling = true;

struct ClientNetProfileState {
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
    uint64_t polls = 0;
    uint64_t messages = 0;
    uint64_t bytes = 0;
    int64_t totalPollUs = 0;
    int64_t totalCallbackUs = 0;
    int64_t totalRecvUs = 0;
    int64_t maxPollUs = 0;
};

ClientNetProfileState& GetClientNetProfileState()
{
    static ClientNetProfileState state;
    return state;
}

void RecordClientNetProfile(
    ClientNetwork* net,
    int64_t pollUs,
    int64_t callbackUs,
    int64_t recvUs,
    uint64_t messages,
    uint64_t bytes
)
{
    if (!kEnableClientNetProfiling || net == nullptr) {
        return;
    }

    ClientNetProfileState& state = GetClientNetProfileState();
    state.polls += 1;
    state.messages += messages;
    state.bytes += bytes;
    state.totalPollUs += pollUs;
    state.totalCallbackUs += callbackUs;
    state.totalRecvUs += recvUs;
    state.maxPollUs = std::max(state.maxPollUs, pollUs);

    const auto now = std::chrono::steady_clock::now();
    const double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(
        now - state.lastLog
    ).count();
    if (elapsedSec < 1.0) {
        return;
    }

    const double polls = (state.polls > 0) ? static_cast<double>(state.polls) : 1.0;
    const double msgs = static_cast<double>(state.messages);
    const double avgPollMs = static_cast<double>(state.totalPollUs) / (polls * 1000.0);
    const double avgCallbackMs = static_cast<double>(state.totalCallbackUs) / (polls * 1000.0);
    const double avgRecvMs = static_cast<double>(state.totalRecvUs) / (polls * 1000.0);
    const double maxPollMs = static_cast<double>(state.maxPollUs) / 1000.0;
    const double avgMsgUs = (state.messages > 0)
        ? (static_cast<double>(state.totalRecvUs) / msgs)
        : 0.0;

    const ClientNetwork::ChunkQueueDepths queueDepths = net->GetChunkQueueDepths();
    std::cerr
        << "[net/profile] polls=" << state.polls
        << " msgs=" << state.messages
        << " bytes=" << state.bytes
        << " pollAvgMs=" << std::fixed << std::setprecision(3) << avgPollMs
        << " pollMaxMs=" << maxPollMs
        << " cbAvgMs=" << avgCallbackMs
        << " recvAvgMs=" << avgRecvMs
        << " msgAvgUs=" << avgMsgUs
        << " queue(data/delta/unload)=("
        << queueDepths.chunkData << "/"
        << queueDepths.chunkDelta << "/"
        << queueDepths.chunkUnload << ")\n";

    state.lastLog = now;
    state.polls = 0;
    state.messages = 0;
    state.bytes = 0;
    state.totalPollUs = 0;
    state.totalCallbackUs = 0;
    state.totalRecvUs = 0;
    state.maxPollUs = 0;
}

template <typename T>
void TrimQueueToDepth(std::deque<T>& queue, size_t maxDepth)
{
    while (queue.size() > maxDepth) {
        queue.pop_front();
    }
}

std::string TrimAscii(std::string value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    if (begin == 0 && end == value.size()) {
        return value;
    }
    return value.substr(begin, end - begin);
}

bool IsValidIdentityChar(char c)
{
    return
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '_' ||
        c == '-';
}

std::string NormalizeIdentity(std::string identity)
{
    identity = TrimAscii(std::move(identity));
    std::string out;
    out.reserve(identity.size());
    for (char c : identity) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (IsValidIdentityChar(lower)) {
            out.push_back(lower);
        }
    }
    return out;
}

bool IsValidIdentity(const std::string& identity)
{
    if (identity.empty() || identity.size() > kMaxConnectIdentityChars) {
        return false;
    }
    for (char c : identity) {
        if (!IsValidIdentityChar(c)) {
            return false;
        }
    }
    return true;
}

std::filesystem::path ResolveIdentityFilePath()
{
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData != nullptr && localAppData[0] != '\0') {
        return std::filesystem::path(localAppData) / "VoxelOps" / kClientIdentityFileName;
    }
    return std::filesystem::current_path() / kClientIdentityFileName;
}

std::string GenerateIdentityToken()
{
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint32_t> dist(0u, 0xFFFFFFFFu);
    std::ostringstream out;
    out << "id-";
    for (int i = 0; i < 5; ++i) {
        const uint32_t part = dist(rng);
        out.width(8);
        out.fill('0');
        out << std::hex << std::nouppercase << part;
    }
    std::string token = out.str();
    if (token.size() > kMaxConnectIdentityChars) {
        token.resize(kMaxConnectIdentityChars);
    }
    return token;
}

bool TryParseKillFeedMessage(const std::string& message, ClientNetwork::KillFeedEvent& out)
{
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
    }
    catch (...) {
        return false;
    }

    out.killer = message.substr(killerStart, killerSep - killerStart);
    out.victim = message.substr(victimStart, victimSep - victimStart);
    out.weaponId = weaponId;
    return !out.killer.empty() && !out.victim.empty();
}

bool ParseIntToken(std::string_view token, int& out)
{
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

bool ParseUint32Token(std::string_view token, uint32_t& out)
{
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

inline bool ReadU8(const uint8_t* data, size_t size, size_t& offset, uint8_t& out)
{
    if (!data || offset + 1 > size) {
        return false;
    }
    out = data[offset++];
    return true;
}

inline bool ReadU16LE(const uint8_t* data, size_t size, size_t& offset, uint16_t& out)
{
    if (!data || offset + 2 > size) {
        return false;
    }
    out = static_cast<uint16_t>(data[offset]) |
        (static_cast<uint16_t>(data[offset + 1]) << 8);
    offset += 2;
    return true;
}

inline bool ReadU32LE(const uint8_t* data, size_t size, size_t& offset, uint32_t& out)
{
    if (!data || offset + 4 > size) {
        return false;
    }
    out = static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

inline bool ReadI32LE(const uint8_t* data, size_t size, size_t& offset, int32_t& out)
{
    uint32_t raw = 0;
    if (!ReadU32LE(data, size, offset, raw)) {
        return false;
    }
    out = static_cast<int32_t>(raw);
    return true;
}

inline bool ReadU64LE(const uint8_t* data, size_t size, size_t& offset, uint64_t& out)
{
    if (!data || offset + 8 > size) {
        return false;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= (static_cast<uint64_t>(data[offset + i]) << (8 * i));
    }
    out = value;
    offset += 8;
    return true;
}

inline bool ReadF32LE(const uint8_t* data, size_t size, size_t& offset, float& out)
{
    uint32_t raw = 0;
    if (!ReadU32LE(data, size, offset, raw)) {
        return false;
    }
    std::memcpy(&out, &raw, sizeof(out));
    return true;
}

bool ParseConnectResponsePacket(const uint8_t* data, size_t size, ConnectResponse& out)
{
    size_t offset = 0;
    uint8_t type = 0;
    uint8_t ok = 0;
    uint8_t reason = 0;
    uint8_t assignedLen = 0;
    uint8_t messageLen = 0;
    uint16_t protocolVersion = 0;
    if (!ReadU8(data, size, offset, type) ||
        type != static_cast<uint8_t>(PacketType::ConnectResponse) ||
        !ReadU8(data, size, offset, ok) ||
        !ReadU8(data, size, offset, reason) ||
        !ReadU16LE(data, size, offset, protocolVersion) ||
        !ReadU8(data, size, offset, assignedLen) ||
        !ReadU8(data, size, offset, messageLen)) {
        return false;
    }
    if (assignedLen > kMaxConnectUsernameChars || messageLen > kMaxConnectMessageChars) {
        return false;
    }
    if (offset + static_cast<size_t>(assignedLen) + static_cast<size_t>(messageLen) != size) {
        return false;
    }

    out.ok = (ok != 0) ? 1u : 0u;
    out.reason = static_cast<ConnectRejectReason>(reason);
    out.serverProtocolVersion = protocolVersion;
    out.assignedUsername.assign(reinterpret_cast<const char*>(data + offset), assignedLen);
    offset += assignedLen;
    out.message.assign(reinterpret_cast<const char*>(data + offset), messageLen);
    return true;
}

bool ParsePlayerSnapshotFramePacket(const uint8_t* data, size_t size, PlayerSnapshotFrame& out)
{
    constexpr size_t kSnapshotEntryBytes = 8 + (8 * 4) + 3 + 2 + 4 + 1 + 4;
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t playerCount = 0;
    if (!ReadU8(data, size, offset, type) ||
        type != static_cast<uint8_t>(PacketType::PlayerSnapshot) ||
        !ReadU32LE(data, size, offset, out.serverTick) ||
        !ReadU64LE(data, size, offset, out.selfPlayerId) ||
        !ReadU32LE(data, size, offset, out.lastProcessedInputSequence) ||
        !ReadU32LE(data, size, offset, playerCount)) {
        return false;
    }

    const size_t bytesRemaining = (offset <= size) ? (size - offset) : 0;
    if (playerCount > (bytesRemaining / kSnapshotEntryBytes)) {
        return false;
    }

    out.players.clear();
    out.players.reserve(playerCount);
    for (uint32_t i = 0; i < playerCount; ++i) {
        PlayerSnapshot snapshot{};
        if (!ReadU64LE(data, size, offset, snapshot.id) ||
            !ReadF32LE(data, size, offset, snapshot.px) ||
            !ReadF32LE(data, size, offset, snapshot.py) ||
            !ReadF32LE(data, size, offset, snapshot.pz) ||
            !ReadF32LE(data, size, offset, snapshot.vx) ||
            !ReadF32LE(data, size, offset, snapshot.vy) ||
            !ReadF32LE(data, size, offset, snapshot.vz) ||
            !ReadF32LE(data, size, offset, snapshot.yaw) ||
            !ReadF32LE(data, size, offset, snapshot.pitch) ||
            !ReadU8(data, size, offset, snapshot.onGround) ||
            !ReadU8(data, size, offset, snapshot.flyMode) ||
            !ReadU8(data, size, offset, snapshot.allowFlyMode) ||
            !ReadU16LE(data, size, offset, snapshot.weaponId) ||
            !ReadF32LE(data, size, offset, snapshot.health) ||
            !ReadU8(data, size, offset, snapshot.isAlive) ||
            !ReadF32LE(data, size, offset, snapshot.respawnSeconds)) {
            return false;
        }
        out.players.push_back(snapshot);
    }

    return offset == size;
}

bool ParseChunkDataPacket(const uint8_t* data, size_t size, ChunkData& out)
{
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t payloadSize = 0;
    if (!ReadU8(data, size, offset, type) ||
        type != static_cast<uint8_t>(PacketType::ChunkData) ||
        !ReadI32LE(data, size, offset, out.chunkX) ||
        !ReadI32LE(data, size, offset, out.chunkY) ||
        !ReadI32LE(data, size, offset, out.chunkZ) ||
        !ReadU64LE(data, size, offset, out.version) ||
        !ReadU8(data, size, offset, out.flags) ||
        !ReadU32LE(data, size, offset, payloadSize)) {
        return false;
    }
    if (offset + payloadSize != size) {
        return false;
    }
    out.payload.resize(payloadSize);
    if (payloadSize > 0) {
        std::memcpy(out.payload.data(), data + offset, payloadSize);
    }
    return true;
}

bool ParseChunkDeltaPacket(const uint8_t* data, size_t size, ChunkDelta& out)
{
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t editCount = 0;
    if (!ReadU8(data, size, offset, type) ||
        type != static_cast<uint8_t>(PacketType::ChunkDelta) ||
        !ReadI32LE(data, size, offset, out.chunkX) ||
        !ReadI32LE(data, size, offset, out.chunkY) ||
        !ReadI32LE(data, size, offset, out.chunkZ) ||
        !ReadU64LE(data, size, offset, out.resultingVersion) ||
        !ReadU32LE(data, size, offset, editCount)) {
        return false;
    }
    if (editCount > ((size - offset) / 4)) {
        return false;
    }

    out.edits.clear();
    out.edits.reserve(editCount);
    for (uint32_t i = 0; i < editCount; ++i) {
        ChunkDeltaOp op{};
        if (!ReadU8(data, size, offset, op.x) ||
            !ReadU8(data, size, offset, op.y) ||
            !ReadU8(data, size, offset, op.z) ||
            !ReadU8(data, size, offset, op.blockId)) {
            return false;
        }
        out.edits.push_back(op);
    }
    return offset == size;
}

bool ParseChunkUnloadPacket(const uint8_t* data, size_t size, ChunkUnload& out)
{
    size_t offset = 0;
    uint8_t type = 0;
    if (!ReadU8(data, size, offset, type) ||
        type != static_cast<uint8_t>(PacketType::ChunkUnload) ||
        !ReadI32LE(data, size, offset, out.chunkX) ||
        !ReadI32LE(data, size, offset, out.chunkY) ||
        !ReadI32LE(data, size, offset, out.chunkZ)) {
        return false;
    }
    return offset == size;
}

bool ParseShootResultPacket(const uint8_t* data, size_t size, ShootResult& out)
{
    size_t offset = 0;
    uint8_t type = 0;
    uint32_t entityRaw = 0;
    if (!ReadU8(data, size, offset, type) ||
        type != static_cast<uint8_t>(PacketType::ShootResult) ||
        !ReadU32LE(data, size, offset, out.clientShotId) ||
        !ReadU32LE(data, size, offset, out.serverTick) ||
        !ReadU8(data, size, offset, out.accepted) ||
        !ReadU8(data, size, offset, out.didHit) ||
        !ReadU32LE(data, size, offset, entityRaw) ||
        !ReadF32LE(data, size, offset, out.hitX) ||
        !ReadF32LE(data, size, offset, out.hitY) ||
        !ReadF32LE(data, size, offset, out.hitZ) ||
        !ReadF32LE(data, size, offset, out.normalX) ||
        !ReadF32LE(data, size, offset, out.normalY) ||
        !ReadF32LE(data, size, offset, out.normalZ) ||
        !ReadF32LE(data, size, offset, out.damageApplied) ||
        !ReadU16LE(data, size, offset, out.newAmmoCount) ||
        !ReadU32LE(data, size, offset, out.serverSeed)) {
        return false;
    }
    out.hitEntityId = static_cast<int32_t>(entityRaw);
    return offset == size;
}

bool ResolveHostToAddress(std::string_view host, uint16_t port, SteamNetworkingIPAddr& outAddr)
{
    if (host.empty()) {
        return false;
    }

    std::string hostStr(host);

    // Fast path for literal IPv4/IPv6 addresses.
    SteamNetworkingIPAddr parsedAddr;
    parsedAddr.Clear();
    if (parsedAddr.ParseString(hostStr.c_str())) {
        parsedAddr.m_port = port;
        outAddr = parsedAddr;
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* results = nullptr;
    const int gaiError = getaddrinfo(hostStr.c_str(), nullptr, &hints, &results);
    if (gaiError != 0 || results == nullptr) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
        return false;
    }

    bool found = false;
    char addressBuffer[INET6_ADDRSTRLEN] = {};
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        const void* rawAddress = nullptr;
        if (it->ai_family == AF_INET) {
            const sockaddr_in* addr4 = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            rawAddress = &addr4->sin_addr;
        }
        else if (it->ai_family == AF_INET6) {
            const sockaddr_in6* addr6 = reinterpret_cast<const sockaddr_in6*>(it->ai_addr);
            rawAddress = &addr6->sin6_addr;
        }
        else {
            continue;
        }

        if (inet_ntop(it->ai_family, rawAddress, addressBuffer, sizeof(addressBuffer)) == nullptr) {
            continue;
        }

        SteamNetworkingIPAddr addr;
        addr.Clear();
        if (!addr.ParseString(addressBuffer)) {
            continue;
        }

        addr.m_port = port;
        outAddr = addr;
        found = true;
        break;
    }

    freeaddrinfo(results);
    return found;
}

bool TryParseScoreboardMessage(const std::string& message, ClientNetwork::ScoreboardSnapshot& out)
{
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
        if (ParseIntToken(fields[2], parsedStarted) && (parsedStarted == 0 || parsedStarted == 1)) {
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
    snapshot.winner = (fields[winnerIndex] == "-") ? std::string() : std::string(fields[winnerIndex]);
    snapshot.entries.reserve(static_cast<size_t>(expectedCount));

    for (size_t i = 0; i < static_cast<size_t>(expectedCount); ++i) {
        const std::string_view entryField = fields[entriesStartIndex + i];
        const size_t c1 = entryField.find(',');
        if (c1 == std::string_view::npos) return false;
        const size_t c2 = entryField.find(',', c1 + 1);
        if (c2 == std::string_view::npos) return false;
        const size_t c3 = entryField.find(',', c2 + 1);
        if (c3 == std::string_view::npos) return false;
        if (entryField.find(',', c3 + 1) != std::string_view::npos) return false;

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
}

ClientNetwork::ClientNetwork() = default;

ClientNetwork::~ClientNetwork() {
    Shutdown();
}

bool ClientNetwork::Start() {
    if (m_started.load()) return true;
    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
        std::cerr << "GNS init failed: " << err << "\n";
        SetConnectionStatus(ConnectionState::Disconnected, "network init failed", false);
        return false;
    }
    m_started = true;
    SetConnectionStatus(ConnectionState::Disconnected, "network initialized", false);
    return true;
}

bool ClientNetwork::ConnectTo(std::string_view host, uint16_t port) {
    if (!m_started.load()) {
        std::cerr << "ClientNetwork: Start() must be called first\n";
        SetConnectionStatus(ConnectionState::Disconnected, "network not started");
        return false;
    }

    std::string hostTrimmed(host);
    size_t begin = 0;
    while (begin < hostTrimmed.size() && std::isspace(static_cast<unsigned char>(hostTrimmed[begin])) != 0) {
        ++begin;
    }
    size_t end = hostTrimmed.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(hostTrimmed[end - 1])) != 0) {
        --end;
    }
    hostTrimmed = hostTrimmed.substr(begin, end - begin);
    if (hostTrimmed.empty()) {
        std::cerr << "ConnectTo: empty host\n";
        SetConnectionStatus(ConnectionState::Disconnected, "invalid server host");
        return false;
    }

    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "reconnect", false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    m_registered = false;
    m_assignedUsername.clear();
    m_allowAutoReconnect = true;

    SteamNetworkingIPAddr addr;
    addr.Clear();
    if (!ResolveHostToAddress(hostTrimmed, port, addr)) {
        std::cerr << "ConnectTo: failed to resolve host '" << hostTrimmed << "'\n";
        SetConnectionStatus(ConnectionState::Disconnected, "failed to resolve server host");
        return false;
    }

    // connect (no extra options)
    m_conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    if (m_conn == k_HSteamNetConnection_Invalid) {
        std::cerr << "ConnectByIPAddress failed\n";
        SetConnectionStatus(ConnectionState::Disconnected, "connect attempt failed");
        return false;
    }
    {
        std::ostringstream status;
        status << "connecting to " << hostTrimmed << ":" << port;
        SetConnectionStatus(ConnectionState::Connecting, status.str());
    }
    return true;
}

bool ClientNetwork::SendConnectRequest(std::string_view requestedUsername) {
    if (m_conn == k_HSteamNetConnection_Invalid) {
        std::cerr << "SendConnectRequest: no connection\n";
        SetConnectionStatus(ConnectionState::Disconnected, "no active connection");
        return false;
    }
    if (requestedUsername.size() > kMaxConnectUsernameChars) {
        std::cerr << "SendConnectRequest: username too long (max 32 chars)\n";
        return false;
    }
    if (!EnsureClientIdentity()) {
        std::cerr << "SendConnectRequest: failed to prepare client identity\n";
        SetConnectionStatus(ConnectionState::Disconnected, "failed to prepare identity", false);
        return false;
    }

    ConnectRequest req;
    req.protocolVersion = kVoxelOpsProtocolVersion;
    req.identity = m_clientIdentity;
    req.requestedUsername.assign(requestedUsername.begin(), requestedUsername.end());
    const std::vector<uint8_t> out = req.serialize();

    EResult r = SteamNetworkingSockets()->SendMessageToConnection(m_conn, out.data(), (uint32_t)out.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    if (r != k_EResultOK) {
        std::cerr << "SendConnectRequest: SendMessageToConnection failed: " << r << "\n";
        SetConnectionStatus(ConnectionState::Disconnected, "failed to send connect request");
        return false;
    }
    SetConnectionStatus(ConnectionState::Connecting, "waiting for server registration");
    return true;
}

bool ClientNetwork::SendPosition(uint32_t seq, const glm::vec3& pos, const glm::vec3& vel) {
    if (m_conn == k_HSteamNetConnection_Invalid) return false;
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(PacketType::PlayerPosition));
    AppendUint32LE(out, seq);
    AppendFloatLE(out, pos.x);
    AppendFloatLE(out, pos.y);
    AppendFloatLE(out, pos.z);
    AppendFloatLE(out, vel.x);
    AppendFloatLE(out, vel.y);
    AppendFloatLE(out, vel.z);
    EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        (uint32_t)out.size(),
        k_nSteamNetworkingSend_UnreliableNoDelay,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendPlayerInput(const PlayerInput& input)
{
    if (!IsConnected()) return false;

    const std::vector<uint8_t> out = input.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        static_cast<uint32_t>(out.size()),
        k_nSteamNetworkingSend_UnreliableNoDelay,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendRespawnRequest()
{
    if (!IsConnected()) return false;

    static constexpr char kPayload[] = "RESPAWN";
    std::vector<uint8_t> out;
    out.reserve(1 + sizeof(kPayload) - 1);
    out.push_back(static_cast<uint8_t>(PacketType::Message));
    out.insert(out.end(), kPayload, kPayload + (sizeof(kPayload) - 1));

    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        static_cast<uint32_t>(out.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendChunkRequest(const glm::ivec3& centerChunk, uint16_t viewDistance)
{
    if (!IsConnected()) return false;

    ChunkRequest request;
    request.chunkX = centerChunk.x;
    request.chunkY = centerChunk.y;
    request.chunkZ = centerChunk.z;
    request.viewDistance = viewDistance;

    std::vector<uint8_t> out = request.serialize();
    EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        (uint32_t)out.size(),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendChunkDataAck(const ChunkData& packet)
{
    if (m_conn == k_HSteamNetConnection_Invalid) return false;

    ChunkAck ack;
    ack.ackedType = static_cast<uint8_t>(PacketType::ChunkData);
    ack.sequence = fnv1a32(packet.payload.data(), packet.payload.size());
    ack.chunkX = packet.chunkX;
    ack.chunkY = packet.chunkY;
    ack.chunkZ = packet.chunkZ;
    ack.version = packet.version;

    const std::vector<uint8_t> ackBuf = ack.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        ackBuf.data(),
        static_cast<uint32_t>(ackBuf.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    if (r != k_EResultOK) {
        std::cerr
            << "[chunk/ack] failed to send ChunkData ACK result=" << r
            << " chunk=(" << packet.chunkX << "," << packet.chunkY << "," << packet.chunkZ << ")"
            << " version=" << packet.version << "\n";
    }
    return (r == k_EResultOK);
}

void ClientNetwork::Poll() {
    if (!m_started.load()) return;

    const auto pollTotalStart = std::chrono::steady_clock::now();

    // run callbacks (connection state, etc.)
    SteamNetworkingSockets()->RunCallbacks();
    const auto afterCallbacks = std::chrono::steady_clock::now();
    const int64_t callbackUs = std::chrono::duration_cast<std::chrono::microseconds>(
        afterCallbacks - pollTotalStart
    ).count();

    // receive messages on the connection (drain)
    if (m_conn == k_HSteamNetConnection_Invalid) {
        const auto pollTotalEnd = std::chrono::steady_clock::now();
        const int64_t pollUs = std::chrono::duration_cast<std::chrono::microseconds>(
            pollTotalEnd - pollTotalStart
        ).count();
        RecordClientNetProfile(this, pollUs, callbackUs, 0, 0, 0);
        return;
    }
    SteamNetConnectionInfo_t info{};
    if (SteamNetworkingSockets()->GetConnectionInfo(m_conn, &info)) {
        if (info.m_eState == k_ESteamNetworkingConnectionState_Connected && m_registered) {
            if (m_assignedUsername.empty()) {
                SetConnectionStatus(ConnectionState::Connected, "connected");
            }
            else {
                SetConnectionStatus(ConnectionState::Connected, std::string("connected as ") + m_assignedUsername);
            }
        }
        else if (info.m_eState == k_ESteamNetworkingConnectionState_Connecting && !m_registered) {
            if (m_connectionState != ConnectionState::Connecting) {
                SetConnectionStatus(ConnectionState::Connecting, "connecting");
            }
        }
        else if (
            info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
            ) {
            std::string reason = "connection closed";
            if (info.m_szEndDebug[0] != '\0') {
                reason = info.m_szEndDebug;
            }
            SteamNetworkingSockets()->CloseConnection(m_conn, 0, reason.c_str(), false);
            m_conn = k_HSteamNetConnection_Invalid;
            m_registered = false;
            m_assignedUsername.clear();
            SetConnectionStatus(ConnectionState::Disconnected, reason);
            const auto pollTotalEnd = std::chrono::steady_clock::now();
            const int64_t pollUs = std::chrono::duration_cast<std::chrono::microseconds>(
                pollTotalEnd - pollTotalStart
            ).count();
            RecordClientNetProfile(this, pollUs, callbackUs, 0, 0, 0);
            return;
        }
    }
    const auto pollBudgetStart = std::chrono::steady_clock::now();
    size_t drainedMessages = 0;
    uint64_t drainedBytes = 0;
    SteamNetworkingMessage_t* pMsg = nullptr;
    while (
        drainedMessages < kMaxMessagesPerPoll &&
        SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_conn, &pMsg, 1) > 0 &&
        pMsg
    ) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(pMsg->m_pData);
        uint32_t cb = pMsg->m_cbSize;
        if (cb >= 1) {
            OnMessage(data, cb);
            drainedBytes += cb;
        }
        pMsg->Release();
        ++drainedMessages;
        const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - pollBudgetStart
        ).count();
        if (elapsedUs >= kMessagePollBudgetUs) {
            break;
        }
    }

    const auto pollTotalEnd = std::chrono::steady_clock::now();
    const int64_t pollUs = std::chrono::duration_cast<std::chrono::microseconds>(
        pollTotalEnd - pollTotalStart
    ).count();
    const int64_t recvUs = std::chrono::duration_cast<std::chrono::microseconds>(
        pollTotalEnd - pollBudgetStart
    ).count();
    RecordClientNetProfile(this, pollUs, callbackUs, recvUs, drainedMessages, drainedBytes);
}

void ClientNetwork::Shutdown() {
    if (m_conn != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(m_conn, 0, "client shutdown", false);
        m_conn = k_HSteamNetConnection_Invalid;
    }
    if (m_started.load()) {
        GameNetworkingSockets_Kill();
        m_started = false;
    }
    m_registered = false;
    m_assignedUsername.clear();
    SetConnectionStatus(ConnectionState::Disconnected, "disconnected");

    {
        std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
        m_chunkDataQueue.clear();
        m_chunkDeltaQueue.clear();
        m_chunkUnloadQueue.clear();
        m_playerSnapshotQueue.clear();
        m_shootResultQueue.clear();
        m_killFeedQueue.clear();
        m_scoreboardQueue.clear();
    }
}

// --- helpers ---
void ClientNetwork::AppendUint32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((v >> 0) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}
void ClientNetwork::AppendFloatLE(std::vector<uint8_t>& out, float f) {
    uint32_t u;
    static_assert(sizeof(u) == sizeof(f), "float size mismatch");
    std::memcpy(&u, &f, sizeof(u));
    AppendUint32LE(out, u);
}
uint32_t ClientNetwork::ReadUint32LE(const uint8_t* ptr) {
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}
float ClientNetwork::ReadFloatLE(const uint8_t* ptr) {
    uint32_t u = ReadUint32LE(ptr);
    float f; std::memcpy(&f, &u, sizeof(f));
    return f;
}

void ClientNetwork::OnMessage(const uint8_t* data, uint32_t size) {
    uint8_t t = data[0];
    if (static_cast<PacketType>(t) == PacketType::ConnectResponse) {
        ConnectResponse resp;
        if (!ParseConnectResponsePacket(data, size, resp)) {
            std::cerr << "[net] malformed ConnectResponse\n";
            m_registered = false;
            m_assignedUsername.clear();
            SetConnectionStatus(ConnectionState::Disconnected, "malformed connect response", false);
            if (m_conn != k_HSteamNetConnection_Invalid) {
                SteamNetworkingSockets()->CloseConnection(m_conn, 0, "malformed connect response", false);
                m_conn = k_HSteamNetConnection_Invalid;
            }
            return;
        }

        if (resp.ok != 0) {
            m_registered = true;
            m_assignedUsername = resp.assignedUsername;
            const std::string displayName = m_assignedUsername.empty() ? std::string("connected") : ("connected as " + m_assignedUsername);
            SetConnectionStatus(ConnectionState::Connected, displayName);
            std::cout << "[net] registered by server";
            if (!m_assignedUsername.empty()) {
                std::cout << " as " << m_assignedUsername;
            }
            std::cout << "\n";
        }
        else {
            m_registered = false;
            m_assignedUsername.clear();
            std::string reason = resp.message.empty() ? std::string("registration rejected") : resp.message;
            std::cout << "[net] registration rejected by server: " << reason << "\n";

            if (resp.reason == ConnectRejectReason::IdentityInUse) {
                // Allow multiple local clients by falling back to a per-process transient identity.
                m_useTransientIdentity = true;
                m_clientIdentity.clear();
                std::cout << "[net] identity conflict detected; rotating to transient identity for retry\n";
            }

            const bool fatalMismatch =
                (resp.reason == ConnectRejectReason::ProtocolMismatch) ||
                (resp.reason == ConnectRejectReason::InvalidIdentity) ||
                (resp.reason == ConnectRejectReason::UsernameTaken);
            SetConnectionStatus(ConnectionState::Disconnected, reason, !fatalMismatch);

            if (m_conn != k_HSteamNetConnection_Invalid) {
                SteamNetworkingSockets()->CloseConnection(m_conn, 0, reason.c_str(), false);
                m_conn = k_HSteamNetConnection_Invalid;
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::Message) {
        // simple text message from server
        if (size > 1) {
            std::string s(reinterpret_cast<const char*>(data + 1), size - 1);
            KillFeedEvent killEvent;
            if (TryParseKillFeedMessage(s, killEvent)) {
                std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
                m_killFeedQueue.push_back(std::move(killEvent));
                TrimQueueToDepth(m_killFeedQueue, kMaxKillFeedQueueDepth);
                return;
            }

            ScoreboardSnapshot scoreboardSnapshot;
            if (TryParseScoreboardMessage(s, scoreboardSnapshot)) {
                std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
                m_scoreboardQueue.push_back(std::move(scoreboardSnapshot));
                TrimQueueToDepth(m_scoreboardQueue, kMaxScoreboardQueueDepth);
                return;
            }
            std::cout << "[server msg] " << s << "\n";
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::PlayerSnapshot) {
        PlayerSnapshotFrame frame;
        if (!ParsePlayerSnapshotFramePacket(data, size, frame)) {
            std::cerr << "[net] malformed PlayerSnapshot\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_playerSnapshotQueue.push_back(std::move(frame));
            // Keep only recent snapshots to bound memory and stale processing.
            while (m_playerSnapshotQueue.size() > 8) {
                m_playerSnapshotQueue.pop_front();
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkData) {
        ChunkData packet;
        if (!ParseChunkDataPacket(data, size, packet)) {
            std::cerr << "[net] malformed ChunkData\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_chunkDataQueue.push_back(std::move(packet));
            TrimQueueToDepth(m_chunkDataQueue, kMaxChunkDataQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkDelta) {
        ChunkDelta packet;
        if (!ParseChunkDeltaPacket(data, size, packet)) {
            std::cerr << "[net] malformed ChunkDelta\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_chunkDeltaQueue.push_back(std::move(packet));
            TrimQueueToDepth(m_chunkDeltaQueue, kMaxChunkDeltaQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkUnload) {
        ChunkUnload packet;
        if (!ParseChunkUnloadPacket(data, size, packet)) {
            std::cerr << "[net] malformed ChunkUnload\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_chunkUnloadQueue.push_back(std::move(packet));
            TrimQueueToDepth(m_chunkUnloadQueue, kMaxChunkUnloadQueueDepth);
        }
        return;
    }

    // handle ShootResult (server -> client authoritative shot result)
    if (static_cast<PacketType>(t) == PacketType::ShootResult) {
        ShootResult result;
        if (!ParseShootResultPacket(data, size, result)) {
            std::cerr << "[net] malformed ShootResult\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_shootResultQueue.push_back(std::move(result));
            while (m_shootResultQueue.size() > 32) {
                m_shootResultQueue.pop_front();
            }
        }
        return;
    }
}



bool ClientNetwork::SendShootRequest(uint32_t clientShotId, uint32_t clientTick, uint16_t weaponId,
    const glm::vec3& pos, const glm::vec3& dir,
    uint32_t seed, uint8_t inputFlags)
{
    if (!IsConnected()) return false;

    ShootRequest req;
    req.clientShotId = clientShotId;
    req.clientTick = clientTick;
    req.weaponId = weaponId;
    req.posX = pos.x; req.posY = pos.y; req.posZ = pos.z;
    req.dirX = dir.x; req.dirY = dir.y; req.dirZ = dir.z;
    req.seed = seed;
    req.inputFlags = inputFlags;

    std::vector<uint8_t> buf = req.serialize();
    // Use reliable for simplicity (authoritative events); you can switch to unreliable if you add retries
    EResult r = SteamNetworkingSockets()->SendMessageToConnection(m_conn, buf.data(), (uint32_t)buf.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    return (r == k_EResultOK);
}

bool ClientNetwork::IsConnected() const
{
    if (!m_started.load() || m_conn == k_HSteamNetConnection_Invalid || !m_registered) {
        return false;
    }

    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(m_conn, &info)) {
        return false;
    }
    return info.m_eState == k_ESteamNetworkingConnectionState_Connected;
}

ClientNetwork::ConnectionState ClientNetwork::GetConnectionState() const noexcept
{
    return m_connectionState;
}

const std::string& ClientNetwork::GetConnectionStatusText() const noexcept
{
    return m_connectionStatus;
}

const std::string& ClientNetwork::GetAssignedUsername() const noexcept
{
    return m_assignedUsername;
}

bool ClientNetwork::ShouldAutoReconnect() const noexcept
{
    return m_allowAutoReconnect;
}

int ClientNetwork::GetPingMs() const noexcept
{
    if (!IsConnected()) {
        return -1;
    }

    SteamNetConnectionRealTimeStatus_t status{};
    const EResult r = SteamNetworkingSockets()->GetConnectionRealTimeStatus(
        m_conn,
        &status,
        0,
        nullptr
    );
    if (r != k_EResultOK) {
        return -1;
    }

    return status.m_nPing;
}

bool ClientNetwork::PopChunkData(ChunkData& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_chunkDataQueue.empty()) {
        return false;
    }
    out = std::move(m_chunkDataQueue.front());
    m_chunkDataQueue.pop_front();
    return true;
}

bool ClientNetwork::PopChunkDelta(ChunkDelta& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_chunkDeltaQueue.empty()) {
        return false;
    }
    out = std::move(m_chunkDeltaQueue.front());
    m_chunkDeltaQueue.pop_front();
    return true;
}

bool ClientNetwork::PopChunkUnload(ChunkUnload& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_chunkUnloadQueue.empty()) {
        return false;
    }
    out = std::move(m_chunkUnloadQueue.front());
    m_chunkUnloadQueue.pop_front();
    return true;
}

bool ClientNetwork::PopPlayerSnapshot(PlayerSnapshotFrame& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_playerSnapshotQueue.empty()) {
        return false;
    }
    out = std::move(m_playerSnapshotQueue.front());
    m_playerSnapshotQueue.pop_front();
    return true;
}

bool ClientNetwork::PopShootResult(ShootResult& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_shootResultQueue.empty()) {
        return false;
    }
    out = std::move(m_shootResultQueue.front());
    m_shootResultQueue.pop_front();
    return true;
}

bool ClientNetwork::PopKillFeedEvent(KillFeedEvent& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_killFeedQueue.empty()) {
        return false;
    }
    out = std::move(m_killFeedQueue.front());
    m_killFeedQueue.pop_front();
    return true;
}

bool ClientNetwork::PopScoreboardSnapshot(ScoreboardSnapshot& out)
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    if (m_scoreboardQueue.empty()) {
        return false;
    }
    out = std::move(m_scoreboardQueue.front());
    m_scoreboardQueue.pop_front();
    return true;
}

ClientNetwork::ChunkQueueDepths ClientNetwork::GetChunkQueueDepths()
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    ChunkQueueDepths depths;
    depths.chunkData = m_chunkDataQueue.size();
    depths.chunkDelta = m_chunkDeltaQueue.size();
    depths.chunkUnload = m_chunkUnloadQueue.size();
    return depths;
}

bool ClientNetwork::EnsureClientIdentity()
{
    if (IsValidIdentity(m_clientIdentity)) {
        return true;
    }

    if (m_useTransientIdentity) {
        std::string transient = NormalizeIdentity(GenerateIdentityToken());
        if (!IsValidIdentity(transient)) {
            return false;
        }
        m_clientIdentity = std::move(transient);
        return true;
    }

    const std::filesystem::path identityPath = ResolveIdentityFilePath();
    std::error_code ec;
    if (!identityPath.parent_path().empty()) {
        std::filesystem::create_directories(identityPath.parent_path(), ec);
    }

    std::string loaded;
    {
        std::ifstream in(identityPath);
        if (in) {
            std::getline(in, loaded);
        }
    }
    loaded = NormalizeIdentity(std::move(loaded));
    if (!IsValidIdentity(loaded)) {
        loaded = NormalizeIdentity(GenerateIdentityToken());
    }
    if (!IsValidIdentity(loaded)) {
        return false;
    }

    {
        std::ofstream out(identityPath, std::ios::out | std::ios::trunc);
        if (out) {
            out << loaded << "\n";
        }
    }
    m_clientIdentity = loaded;
    return true;
}

void ClientNetwork::SetConnectionStatus(ConnectionState state, std::string text, bool allowReconnect)
{
    m_connectionState = state;
    m_connectionStatus = std::move(text);
    m_allowAutoReconnect = allowReconnect;
}


