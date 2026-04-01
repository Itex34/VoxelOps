#include "ServerNetwork.hpp"
#include "ValidationSystem.hpp"

#include <charconv>
#include <string_view>
#include <system_error>

namespace {

constexpr auto kInboundRateWindow = std::chrono::seconds(1);
constexpr uint32_t kMaxInboundPacketsPerWindow = 900u;
constexpr uint32_t kMaxInboundBytesPerWindow = 256u * 1024u;
constexpr uint32_t kMaxPlayerInputsPerWindow = 360u;
constexpr uint32_t kMaxChunkRequestsPerWindow = 120u;

} // namespace

bool ServerNetwork::IsInboundRateLimitExceeded(HSteamNetConnection incoming, PacketType packetType, uint32_t bytes)
{
    bool exceededRateLimit = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            auto& session = it->second;
            const auto now = std::chrono::steady_clock::now();
            if (session.inboundRateWindowStart == std::chrono::steady_clock::time_point::min() ||
                (now - session.inboundRateWindowStart) >= kInboundRateWindow) {
                session.inboundRateWindowStart = now;
                session.inboundPacketsInWindow = 0;
                session.inboundBytesInWindow = 0;
                session.inboundPlayerInputsInWindow = 0;
                session.inboundChunkRequestsInWindow = 0;
            }

            ++session.inboundPacketsInWindow;
            session.inboundBytesInWindow += bytes;
            if (packetType == PacketType::PlayerInput) {
                ++session.inboundPlayerInputsInWindow;
            }
            if (packetType == PacketType::ChunkRequest) {
                ++session.inboundChunkRequestsInWindow;
            }

            exceededRateLimit =
                (session.inboundPacketsInWindow > kMaxInboundPacketsPerWindow) ||
                (session.inboundBytesInWindow > kMaxInboundBytesPerWindow) ||
                (session.inboundPlayerInputsInWindow > kMaxPlayerInputsPerWindow) ||
                (session.inboundChunkRequestsInWindow > kMaxChunkRequestsPerWindow);
        }
    }
    if (exceededRateLimit) {
        std::cerr << "[recv] inbound rate limit exceeded conn=" << incoming << " (closing connection)\n";
        SteamNetworkingSockets()->CloseConnection(incoming, 0, "rate limit exceeded", false);
        return true;
    }
    return false;
}

void ServerNetwork::HandleConnectRequest(HSteamNetConnection incoming, const void* data, uint32_t size)
{
    auto sendResponse = [&](const ConnectResponse& response) {
        const std::vector<uint8_t> payload = response.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming,
            payload.data(),
            static_cast<uint32_t>(payload.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    };

    std::vector<uint8_t> connectBytes(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
    auto reqOpt = ConnectRequest::deserialize(connectBytes);
    if (!reqOpt.has_value()) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::InvalidPacket;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "invalid connect packet";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=invalid_packet\n";
        return;
    }

    ConnectRequest req = std::move(*reqOpt);
    if (req.protocolVersion != kVoxelOpsProtocolVersion) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::ProtocolMismatch;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "protocol mismatch: client=" + std::to_string(req.protocolVersion) +
            " server=" + std::to_string(kVoxelOpsProtocolVersion);
        sendResponse(response);
        std::cout
            << "[register rejected] conn=" << incoming
            << " reason=protocol_mismatch client=" << req.protocolVersion
            << " server=" << kVoxelOpsProtocolVersion << "\n";
        return;
    }

    const std::string identity = NetValidation::NormalizeIdentity(req.identity);
    if (!NetValidation::IsValidIdentity(identity)) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::InvalidIdentity;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "invalid identity";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=invalid_identity\n";
        return;
    }

    const std::string requestedUsername = NetValidation::NormalizeDisplayName(req.requestedUsername);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end() && it->second.playerId != 0 && !it->second.username.empty()) {
            ConnectResponse response;
            response.ok = 1;
            response.reason = ConnectRejectReason::None;
            response.serverProtocolVersion = kVoxelOpsProtocolVersion;
            response.assignedUsername = it->second.username;
            response.message = "already registered";
            sendResponse(response);
            std::cout
                << "[register] duplicate ConnectRequest ignored conn=" << incoming
                << " username=" << it->second.username << "\n";
            return;
        }

        for (const auto& [conn, session] : m_clients) {
            if (conn == incoming || session.playerId == 0 || session.identity.empty()) {
                continue;
            }
            if (session.identity == identity) {
                ConnectResponse response;
                response.ok = 0;
                response.reason = ConnectRejectReason::IdentityInUse;
                response.serverProtocolVersion = kVoxelOpsProtocolVersion;
                response.message = "identity already connected";
                sendResponse(response);
                std::cout
                    << "[register rejected] conn=" << incoming
                    << " identity=" << identity
                    << " reason=identity_in_use\n";
                return;
            }
        }

        if (!requestedUsername.empty()) {
            for (const auto& [conn, session] : m_clients) {
                if (conn == incoming || session.playerId == 0 || session.username.empty()) {
                    continue;
                }
                if (session.username == requestedUsername) {
                    ConnectResponse response;
                    response.ok = 0;
                    response.reason = ConnectRejectReason::UsernameTaken;
                    response.serverProtocolVersion = kVoxelOpsProtocolVersion;
                    response.message = "username already taken";
                    sendResponse(response);
                    std::cout
                        << "[register rejected] conn=" << incoming
                        << " requested=" << requestedUsername
                        << " reason=username_taken\n";
                    return;
                }
            }
        }
    }

    std::string username;
    if (!requestedUsername.empty()) {
        username = requestedUsername;
    }
    else {
        username = BuildDisplayNameForIdentityLocked(identity, requestedUsername, incoming);
    }
    if (username.empty()) {
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::ServerError;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "failed to allocate display name";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=name_allocation_failed\n";
        return;
    }

    auto connHandle = std::make_shared<ConnectionHandle>();
    connHandle->socketFd = static_cast<int>(incoming);
    const PlayerID playerId = m_playerManager.onPlayerConnect(connHandle, glm::vec3(0.0f, 60.0f, 0.0f));

    bool attached = false;
    bool sessionIsAdmin = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            it->second.identity = identity;
            it->second.username = username;
            it->second.playerId = playerId;
            it->second.isAdmin = (m_adminIdentities.find(identity) != m_adminIdentities.end());
            sessionIsAdmin = it->second.isAdmin;
            attached = true;
            m_matchScores[playerId] = MatchScore{};

            if (!m_matchStarted) {
                size_t activePlayers = 0;
                for (const auto& [_, session] : m_clients) {
                    if (session.playerId != 0) {
                        ++activePlayers;
                    }
                }
                if (activePlayers >= 2) {
                    m_matchStarted = true;
                    m_matchStartTime = std::chrono::steady_clock::now();
                    m_matchEnded = false;
                    m_matchWinner.clear();
                }
            }
        }
    }
    if (!attached) {
        m_playerManager.removePlayer(playerId);
        ConnectResponse response;
        response.ok = 0;
        response.reason = ConnectRejectReason::ServerError;
        response.serverProtocolVersion = kVoxelOpsProtocolVersion;
        response.message = "failed to attach session";
        sendResponse(response);
        std::cout << "[register rejected] conn=" << incoming << " reason=attach_failed\n";
        return;
    }

    m_playerManager.setFlyModeAllowed(playerId, sessionIsAdmin);

    ConnectResponse response;
    response.ok = 1;
    response.reason = ConnectRejectReason::None;
    response.serverProtocolVersion = kVoxelOpsProtocolVersion;
    response.assignedUsername = username;
    response.message = "ok";
    sendResponse(response);

    InventorySnapshot inventorySnapshot{};
    if (m_playerManager.getInventorySnapshot(playerId, inventorySnapshot)) {
        const std::vector<uint8_t> snapshotBytes = inventorySnapshot.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming,
            snapshotBytes.data(),
            static_cast<uint32_t>(snapshotBytes.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    }

    std::string out;
    out.push_back(static_cast<char>(PacketType::ClientConnect));
    out += username;
    BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), incoming);

    std::cout
        << "[register] conn=" << incoming
        << " username=" << username
        << " identity=" << identity
        << " requested=" << requestedUsername << "\n";
}

void ServerNetwork::HandleMessagePacket(HSteamNetConnection incoming, const void* data, uint32_t size)
{
    std::string msg = ReadStringFromPacket(data, size, 1);
    std::string username;
    PlayerID playerId = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_clients.find(incoming);
        if (it != m_clients.end()) {
            username = it->second.username;
            playerId = it->second.playerId;
        }
    }
    if (!username.empty()) {
        if (playerId != 0) {
            m_playerManager.touchHeartbeat(playerId);
        }

        if (msg == "RESPAWN") {
            if (playerId != 0) {
                (void)m_playerManager.requestRespawn(playerId);
            }
            return;
        }

        constexpr std::string_view kChunkResyncPrefix = "CHUNK_RESYNC|";
        if (
            msg.size() > kChunkResyncPrefix.size() &&
            msg.compare(0, kChunkResyncPrefix.size(), kChunkResyncPrefix.data(), kChunkResyncPrefix.size()) == 0
        ) {
            const std::string_view payload(msg.data() + kChunkResyncPrefix.size(), msg.size() - kChunkResyncPrefix.size());
            const size_t firstSep = payload.find('|');
            const size_t secondSep = (firstSep == std::string_view::npos)
                ? std::string_view::npos
                : payload.find('|', firstSep + 1);
            if (firstSep == std::string_view::npos || secondSep == std::string_view::npos) {
                return;
            }

            const std::string_view sx = payload.substr(0, firstSep);
            const std::string_view sy = payload.substr(firstSep + 1, secondSep - firstSep - 1);
            const std::string_view sz = payload.substr(secondSep + 1);

            auto parseI32 = [](std::string_view text, int32_t& out) -> bool {
                if (text.empty()) {
                    return false;
                }
                const char* begin = text.data();
                const char* end = text.data() + text.size();
                const auto parseResult = std::from_chars(begin, end, out);
                return parseResult.ec == std::errc{} && parseResult.ptr == end;
            };

            int32_t cx = 0;
            int32_t cy = 0;
            int32_t cz = 0;
            if (!parseI32(sx, cx) || !parseI32(sy, cy) || !parseI32(sz, cz)) {
                return;
            }

            const glm::ivec3 chunkPos(cx, cy, cz);
            if (!m_chunkManager.inBounds(chunkPos)) {
                return;
            }

            const ChunkCoord coord{ cx, cy, cz };
            if (m_chunkManager.getChunkIfExists(chunkPos) == nullptr) {
                (void)PrepareChunkForStreaming(coord);
            }
            if (SendChunkData(incoming, coord)) {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_clients.find(incoming);
                if (it != m_clients.end()) {
                    it->second.pendingChunkData.erase(coord);
                    it->second.streamedChunks.insert(coord);
                }
            }
            return;
        }

        m_messageHistory.emplace_back(username, msg);
        std::string out;
        out.push_back(static_cast<char>(PacketType::Message));
        out += username;
        out.push_back(':');
        out += msg;
        BroadcastRaw(out.data(), static_cast<uint32_t>(out.size()), incoming);
        std::cout << "[recv] " << username << ": " << msg << "\n";
    }
    else {
        std::cout << "[dropping] message from unregistered conn=" << incoming << "\n";
    }
}

