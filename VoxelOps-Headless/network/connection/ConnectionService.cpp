#include "ConnectionService.hpp"

#include "../protocol/Validation.hpp"

#include <glm/ext/vector_int3.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

namespace {

    constexpr auto kInboundRateWindow = std::chrono::seconds(1);
    constexpr uint32_t kMaxInboundPacketsPerWindow = 900u;
    constexpr uint32_t kMaxInboundBytesPerWindow = 256u * 1024u;
    constexpr uint32_t kMaxPlayerInputsPerWindow = 360u;
    constexpr uint32_t kMaxChunkRequestsPerWindow = 120u;
    constexpr auto kSoftInputRateLimitLogInterval = std::chrono::seconds(2);
    constexpr auto kChunkResyncRateWindow = std::chrono::seconds(1);
    constexpr uint32_t kMaxChunkResyncRequestsPerWindow = 12u;

} // namespace

ConnectionService::ConnectionService(
    SessionState &sessionState,
    PlayerManager &playerManager,
    ChunkManager &chunkManager,
    ChatService &chatService,
    AdminService &adminService,
    HSteamNetPollGroup &pollGroup,
    Hooks hooks
)
    : m_sessionState(sessionState)
    , m_playerManager(playerManager)
    , m_chunkManager(chunkManager)
    , m_chatService(chatService)
    , m_adminService(adminService)
    , m_pollGroup(pollGroup)
    , m_hooks(std::move(hooks)) {}

bool ConnectionService::IsInboundRateLimitExceeded(
    HSteamNetConnection incoming, PacketType packetType, uint32_t bytes
) {
    struct RateLimitSnapshot {
        bool exceeded = false;
        bool closeConnection = false;
        bool log = true;
        uint32_t packets = 0;
        uint32_t bytes = 0;
        uint32_t inputs = 0;
        uint32_t chunkRequests = 0;
    };

    RateLimitSnapshot snapshot{};
    const bool exceededRateLimit = m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        auto it = sessions.find(incoming);
        if (it == sessions.end()) {
            return false;
        }

        auto &session = it->second;
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

        snapshot.packets = session.inboundPacketsInWindow;
        snapshot.bytes = session.inboundBytesInWindow;
        snapshot.inputs = session.inboundPlayerInputsInWindow;
        snapshot.chunkRequests = session.inboundChunkRequestsInWindow;
        snapshot.exceeded = (session.inboundPacketsInWindow > kMaxInboundPacketsPerWindow) ||
                            (session.inboundBytesInWindow > kMaxInboundBytesPerWindow) ||
                            (session.inboundPlayerInputsInWindow > kMaxPlayerInputsPerWindow) ||
                            (session.inboundChunkRequestsInWindow > kMaxChunkRequestsPerWindow);
        const bool softBurstPacket =
            packetType == PacketType::PlayerInput || packetType == PacketType::ChunkRequest;
        snapshot.closeConnection =
            snapshot.exceeded &&
            (session.inboundBytesInWindow > kMaxInboundBytesPerWindow ||
             session.inboundChunkRequestsInWindow > kMaxChunkRequestsPerWindow ||
             (!softBurstPacket && session.inboundPacketsInWindow > kMaxInboundPacketsPerWindow));
        if (snapshot.exceeded && !snapshot.closeConnection) {
            snapshot.log =
                session.lastInboundRateLimitLogTime == std::chrono::steady_clock::time_point::min(
                                                       ) ||
                (now - session.lastInboundRateLimitLogTime) >= kSoftInputRateLimitLogInterval;
            if (snapshot.log) {
                session.lastInboundRateLimitLogTime = now;
            }
        }
        return snapshot.exceeded;
    });
    if (exceededRateLimit) {
        if (snapshot.log) {
            std::cerr << "[recv] inbound rate limit exceeded conn=" << incoming
                      << " type=" << static_cast<int>(packetType)
                      << " packets=" << snapshot.packets << "/" << kMaxInboundPacketsPerWindow
                      << " bytes=" << snapshot.bytes << "/" << kMaxInboundBytesPerWindow
                      << " inputs=" << snapshot.inputs << "/" << kMaxPlayerInputsPerWindow
                      << " chunkReq=" << snapshot.chunkRequests << "/" << kMaxChunkRequestsPerWindow
                      << " lastBytes=" << bytes
                      << (snapshot.closeConnection ? " (closing connection)" : " (dropping packet)")
                      << "\n";
        }
        if (snapshot.closeConnection) {
            SteamNetworkingSockets()->CloseConnection(incoming, 0, "rate limit exceeded", false);
        }
        return true;
    }
    return false;
}

void ConnectionService::ReleasePendingRegistration(HSteamNetConnection conn) {
    m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        sessions.ReleasePendingRegistration(conn);
    });
}

void ConnectionService::EnsureSpawnCollisionChunksPrepared(const glm::vec3 &spawnPos) {
    if (m_spawnCollisionChunksPrepared) {
        return;
    }

    const glm::ivec3 spawnWorldPos(
        static_cast<int>(std::floor(spawnPos.x)),
        static_cast<int>(std::floor(spawnPos.y)),
        static_cast<int>(std::floor(spawnPos.z))
    );
    const glm::ivec3 centerChunk = m_chunkManager.worldToChunkPos(spawnWorldPos);
    constexpr int kSpawnCollisionRadiusXZ = 1;
    constexpr int kSpawnCollisionRadiusY = 1;
    for (int dx = -kSpawnCollisionRadiusXZ; dx <= kSpawnCollisionRadiusXZ; ++dx) {
        for (int dz = -kSpawnCollisionRadiusXZ; dz <= kSpawnCollisionRadiusXZ; ++dz) {
            for (int dy = -kSpawnCollisionRadiusY; dy <= kSpawnCollisionRadiusY; ++dy) {
                const glm::ivec3 chunkPos(
                    centerChunk.x + dx, centerChunk.y + dy, centerChunk.z + dz
                );
                if (!m_chunkManager.inBounds(chunkPos) || m_chunkManager.hasChunkLoaded(chunkPos)) {
                    continue;
                }
                m_chunkManager.generateTerrainChunkAt(chunkPos);
            }
        }
    }
    m_spawnCollisionChunksPrepared = true;
}

void ConnectionService::HandleConnectRequest(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    auto sendResponse = [&](const ConnectResponse &response) {
        const std::vector<uint8_t> payload = response.serialize();
        (void)SteamNetworkingSockets()->SendMessageToConnection(
            incoming,
            payload.data(),
            static_cast<uint32_t>(payload.size()),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    };

    std::vector<uint8_t> connectBytes(
        reinterpret_cast<const uint8_t *>(data), reinterpret_cast<const uint8_t *>(data) + size
    );
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
        std::cout << "[register rejected] conn=" << incoming
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

    const std::string requestedUsername =
        NetValidation::NormalizeDisplayName(req.requestedUsername);

    std::string username;
    ConnectResponse deferredResponse{};
    bool shouldSendDeferredResponse = false;
    std::string deferredLogLine;
    auto queueDeferredResponse =
        [&](ConnectRejectReason reason, std::string message, std::string logLine, bool ok) {
            deferredResponse = ConnectResponse{};
            deferredResponse.ok = ok ? 1 : 0;
            deferredResponse.reason = reason;
            deferredResponse.serverProtocolVersion = kVoxelOpsProtocolVersion;
            deferredResponse.message = std::move(message);
            deferredLogLine = std::move(logLine);
            shouldSendDeferredResponse = true;
        };

    bool registrationRejected = false;
    m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        auto it = sessions.find(incoming);
        if (it == sessions.end()) {
            registrationRejected = true;
            return;
        }
        if (it->second.playerId != 0 && !it->second.username.empty()) {
            queueDeferredResponse(
                ConnectRejectReason::None,
                "already registered",
                "[register] duplicate ConnectRequest ignored conn=" + std::to_string(incoming) +
                    " username=" + it->second.username,
                true
            );
            deferredResponse.assignedUsername = it->second.username;
            registrationRejected = true;
            return;
        }

        // Defensive cleanup for a previously interrupted registration attempt on this connection.
        sessions.ReleasePendingRegistration(incoming);

        for (const auto &[conn, session] : sessions) {
            if (conn == incoming || session.playerId == 0 || session.identity.empty()) {
                continue;
            }
            if (session.identity == identity) {
                queueDeferredResponse(
                    ConnectRejectReason::IdentityInUse,
                    "identity already connected",
                    "[register rejected] conn=" + std::to_string(incoming) +
                        " identity=" + identity + " reason=identity_in_use",
                    false
                );
                registrationRejected = true;
                return;
            }
        }
        if (sessions.IsIdentityPending(identity)) {
            queueDeferredResponse(
                ConnectRejectReason::IdentityInUse,
                "identity currently registering",
                "[register rejected] conn=" + std::to_string(incoming) + " identity=" + identity +
                    " reason=identity_registering",
                false
            );
            registrationRejected = true;
            return;
        }

        if (!requestedUsername.empty()) {
            for (const auto &[conn, session] : sessions) {
                if (conn == incoming || session.playerId == 0 || session.username.empty()) {
                    continue;
                }
                if (session.username == requestedUsername) {
                    queueDeferredResponse(
                        ConnectRejectReason::UsernameTaken,
                        "username already taken",
                        "[register rejected] conn=" + std::to_string(incoming) +
                            " requested=" + requestedUsername + " reason=username_taken",
                        false
                    );
                    registrationRejected = true;
                    return;
                }
            }
            if (sessions.IsUsernamePendingForOtherConnection(incoming, requestedUsername)) {
                queueDeferredResponse(
                    ConnectRejectReason::UsernameTaken,
                    "username currently registering",
                    "[register rejected] conn=" + std::to_string(incoming) +
                        " requested=" + requestedUsername + " reason=username_registering",
                    false
                );
                registrationRejected = true;
                return;
            }
            username = requestedUsername;
        } else {
            username = m_hooks.buildDisplayName(identity, requestedUsername, incoming);
        }
        if (username.empty()) {
            queueDeferredResponse(
                ConnectRejectReason::ServerError,
                "failed to allocate display name",
                "[register rejected] conn=" + std::to_string(incoming) +
                    " reason=name_allocation_failed",
                false
            );
            registrationRejected = true;
            return;
        }

        sessions.MarkPendingRegistration(incoming, identity, username);
    });
    if (shouldSendDeferredResponse) {
        sendResponse(deferredResponse);
        if (!deferredLogLine.empty()) {
            std::cout << deferredLogLine << "\n";
        }
    }
    if (registrationRejected) {
        return;
    }

    constexpr glm::vec3 kSpawnPos(0.0f, 60.0f, 0.0f);
    EnsureSpawnCollisionChunksPrepared(kSpawnPos);

    auto connHandle = std::make_shared<ConnectionHandle>();
    connHandle->socketFd = static_cast<int>(incoming);
    const PlayerID playerId = m_playerManager.onPlayerConnect(connHandle, kSpawnPos);
    m_hooks.invalidateCombatSnapshotCache();

    bool attached = false;
    bool sessionIsAdmin = false;
    size_t activeSessions = 0;
    m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        auto it = sessions.find(incoming);
        if (it != sessions.end() && it->second.playerId == 0) {
            it->second.identity = identity;
            it->second.username = username;
            it->second.playerId = playerId;
            it->second.isAdmin = m_adminService.IsAdminIdentity(identity);
            sessionIsAdmin = it->second.isAdmin;
            attached = true;
            sessions.ReleasePendingRegistration(incoming);
            sessions.BindPlayerConnection(playerId, incoming);
            activeSessions = sessions.CountRegisteredSessions();
        }
    });
    if (attached) {
        m_hooks.onSessionAttached(playerId, activeSessions);
    }
    if (!attached) {
        m_sessionState.WithLock([&](ClientSessionManager &sessions) {
            sessions.ReleasePendingRegistration(incoming);
        });
        m_playerManager.removePlayer(playerId);
        m_hooks.invalidateCombatSnapshotCache();
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
    m_hooks.broadcastRaw(out.data(), static_cast<uint32_t>(out.size()), incoming);

    std::cout << "[register] conn=" << incoming << " username=" << username
              << " identity=" << identity << " requested=" << requestedUsername << "\n";
}

void ConnectionService::HandleMessagePacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    std::string msg = ReadStringFromPacket(data, size, 1);
    std::string username;
    PlayerID playerId = 0;
    m_sessionState.WithLock([&](ClientSessionManager &sessions) {
        auto it = sessions.find(incoming);
        if (it != sessions.end()) {
            username = it->second.username;
            playerId = it->second.playerId;
        }
    });
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
        if (msg.size() > kChunkResyncPrefix.size() &&
            msg.compare(
                0, kChunkResyncPrefix.size(), kChunkResyncPrefix.data(), kChunkResyncPrefix.size()
            ) == 0) {
            const std::string_view payload(
                msg.data() + kChunkResyncPrefix.size(), msg.size() - kChunkResyncPrefix.size()
            );
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

            auto parseI32 = [](std::string_view text, int32_t &out) -> bool {
                if (text.empty()) {
                    return false;
                }
                const char *begin = text.data();
                const char *end = text.data() + text.size();
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

            const ChunkCoord coord{cx, cy, cz};
            bool allowResync = false;
            bool rateLimited = false;
            m_sessionState.WithLock([&](ClientSessionManager &sessions) {
                auto it = sessions.find(incoming);
                if (it == sessions.end() || it->second.playerId == 0 ||
                    it->second.username.empty()) {
                    return;
                }

                auto &session = it->second;
                const auto nowSteady = std::chrono::steady_clock::now();
                if (session.chunkResyncRateWindowStart ==
                        std::chrono::steady_clock::time_point::min() ||
                    (nowSteady - session.chunkResyncRateWindowStart) >= kChunkResyncRateWindow) {
                    session.chunkResyncRateWindowStart = nowSteady;
                    session.chunkResyncRequestsInWindow = 0;
                }

                if (session.chunkResyncRequestsInWindow >= kMaxChunkResyncRequestsPerWindow) {
                    rateLimited = true;
                    return;
                }

                bool plausibleChunk =
                    (session.pendingChunkData.find(coord) != session.pendingChunkData.end()) ||
                    (session.streamedChunks.find(coord) != session.streamedChunks.end());

                if (!plausibleChunk && session.hasChunkInterest) {
                    const int radius = std::max<int>(2, static_cast<int>(session.viewDistance));
                    const int64_t dx =
                        static_cast<int64_t>(coord.x - session.interestCenterChunk.x);
                    const int64_t dz =
                        static_cast<int64_t>(coord.z - session.interestCenterChunk.z);
                    const int64_t radius2 =
                        static_cast<int64_t>(radius) * static_cast<int64_t>(radius);
                    plausibleChunk = (dx * dx + dz * dz) <= radius2;
                }

                if (!plausibleChunk) {
                    return;
                }

                ++session.chunkResyncRequestsInWindow;
                session.streamedChunks.erase(coord);
                session.pendingChunkData[coord] = nowSteady - std::chrono::milliseconds(600);
                session.chunkInterestDirty = true;
                session.nextChunkInterestUpdateAt = std::chrono::steady_clock::time_point::min();
                allowResync = true;
            });

            if (rateLimited) {
                static uint64_t s_chunkResyncRateLimitLogCount = 0;
                ++s_chunkResyncRateLimitLogCount;
                if (s_chunkResyncRateLimitLogCount <= 20 ||
                    (s_chunkResyncRateLimitLogCount % 200) == 0) {
                    std::cerr << "[chunk/resync] rate limited conn=" << incoming << " chunk=(" << cx
                              << "," << cy << "," << cz << ")"
                              << " count=" << s_chunkResyncRateLimitLogCount << "\n";
                }
                return;
            }
            if (!allowResync) {
                return;
            }

            if (m_hooks.queueChunkPreparation) {
                (void)m_hooks.queueChunkPreparation(incoming, coord);
            }
            return;
        }

        m_chatService.AddMessage(username, msg);
        std::string out;
        out.push_back(static_cast<char>(PacketType::Message));
        out += username;
        out.push_back(':');
        out += msg;
        m_hooks.broadcastRaw(out.data(), static_cast<uint32_t>(out.size()), incoming);
        std::cout << "[recv] " << username << ": " << msg << "\n";
    } else {
        std::cout << "[dropping] message from unregistered conn=" << incoming << "\n";
    }
}

void ConnectionService::OnConnectionStatusChanged(
    SteamNetConnectionStatusChangedCallback_t *pInfo
) {
    if (!pInfo) {
        return;
    }

    HSteamNetConnection hConn = pInfo->m_hConn;
    const SteamNetConnectionInfo_t &info = pInfo->m_info;

    if (info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
        EResult res = SteamNetworkingSockets()->AcceptConnection(hConn);
        if (res != k_EResultOK) {
            std::cerr << "[callback] AcceptConnection failed: " << res << " conn=" << hConn << "\n";
            return;
        }

        if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
            bool ok = SteamNetworkingSockets()->SetConnectionPollGroup(hConn, m_pollGroup);
            if (!ok) {
                std::cerr << "[callback] SetConnectionPollGroup failed for conn=" << hConn << "\n";
            }
        }

        m_sessionState.WithLock([&](ClientSessionManager &sessions) {
            (void)sessions.AddConnection(hConn);
        });
        std::cout << "[callback] accepted conn=" << hConn << "\n";
        return;
    }

    if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        ClientSession session{};
        m_sessionState.WithLock([&](ClientSessionManager &sessions) {
            (void)sessions.RemoveConnection(hConn, &session);
        });
        m_hooks.teardownClientSession(hConn, session, "closed by server callback", true);
        std::cout << "[callback] conn closed/failed: conn=" << hConn << " reason=" << info.m_eState
                  << "\n";
        return;
    }
}

std::string
ConnectionService::ReadStringFromPacket(const void *data, std::uint32_t size, std::size_t offset) {
    assert(data != nullptr || size == 0);

    if (data == nullptr || offset >= static_cast<std::size_t>(size)) {
        return {};
    }

    const char *bytes = static_cast<const char *>(data);
    return std::string(bytes + offset, static_cast<std::size_t>(size) - offset);
}
