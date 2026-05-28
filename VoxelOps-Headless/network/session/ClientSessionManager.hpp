#pragma once

#include "../../../Shared/network/Packets.hpp"
#include "../../../Shared/player/PlayerID.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <glm/ext/vector_int3.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ClientSessionManager {
public:
    struct ChunkCoord {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const ChunkCoord &other) const noexcept {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct ChunkCoordHash {
        std::size_t operator()(const ChunkCoord &c) const noexcept {
            const uint64_t x = static_cast<uint32_t>(c.x);
            const uint64_t y = static_cast<uint32_t>(c.y);
            const uint64_t z = static_cast<uint32_t>(c.z);
            const uint64_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
            return static_cast<std::size_t>(h);
        }
    };

    struct ClientSession {
        std::string identity;
        std::string username;
        PlayerID playerId = 0;
        glm::ivec3 interestCenterChunk{0};
        uint16_t viewDistance = 8;
        bool hasChunkInterest = false;
        bool chunkInterestDirty = false;
        std::chrono::steady_clock::time_point nextChunkInterestUpdateAt =
            std::chrono::steady_clock::time_point::min();
        std::unordered_set<ChunkCoord, ChunkCoordHash> streamedChunks;
        std::unordered_map<ChunkCoord, std::chrono::steady_clock::time_point, ChunkCoordHash>
            pendingChunkData;
        bool isAdmin = false;
        std::chrono::steady_clock::time_point inboundRateWindowStart =
            std::chrono::steady_clock::time_point::min();
        uint32_t inboundPacketsInWindow = 0;
        uint32_t inboundBytesInWindow = 0;
        uint32_t inboundPlayerInputsInWindow = 0;
        uint32_t inboundChunkRequestsInWindow = 0;
        std::chrono::steady_clock::time_point lastAcceptedShootTime =
            std::chrono::steady_clock::time_point::min();
        uint32_t lastShootClientShotId = 0;
        bool hasLastShootClientShotId = false;
    };

    using SessionMap = std::unordered_map<HSteamNetConnection, ClientSession>;
    using iterator = SessionMap::iterator;
    using const_iterator = SessionMap::const_iterator;

    iterator begin() noexcept {
        return m_clients.begin();
    }
    const_iterator begin() const noexcept {
        return m_clients.begin();
    }
    iterator end() noexcept {
        return m_clients.end();
    }
    const_iterator end() const noexcept {
        return m_clients.end();
    }
    iterator find(HSteamNetConnection conn) {
        return m_clients.find(conn);
    }
    const_iterator find(HSteamNetConnection conn) const {
        return m_clients.find(conn);
    }
    iterator erase(iterator it) {
        if (it == m_clients.end()) {
            return it;
        }
        const HSteamNetConnection conn = it->first;
        iterator next = m_clients.erase(it);
        ReleasePendingRegistration(conn);
        for (auto idxIt = m_connectionByPlayerId.begin(); idxIt != m_connectionByPlayerId.end();) {
            if (idxIt->second == conn) {
                idxIt = m_connectionByPlayerId.erase(idxIt);
            } else {
                ++idxIt;
            }
        }
        return next;
    }
    template <typename... Args>
    std::pair<iterator, bool> emplace(Args &&...args) {
        return m_clients.emplace(std::forward<Args>(args)...);
    }
    size_t erase(HSteamNetConnection conn);
    void clear();
    size_t size() const noexcept {
        return m_clients.size();
    }
    bool empty() const noexcept {
        return m_clients.empty();
    }
    std::pair<iterator, bool> AddConnection(HSteamNetConnection conn);
    bool RemoveConnection(HSteamNetConnection conn, ClientSession *removedSession = nullptr);
    ClientSession *FindSession(HSteamNetConnection conn);
    const ClientSession *FindSession(HSteamNetConnection conn) const;
    std::vector<std::pair<HSteamNetConnection, ClientSession>> SnapshotSessions() const;
    size_t CountRegisteredSessions() const;

    void ReleasePendingRegistration(HSteamNetConnection conn);
    void ClearPendingRegistrationState();
    void MarkPendingRegistration(
        HSteamNetConnection conn, std::string identity, std::string username
    );
    bool IsIdentityPending(std::string_view identity) const;
    bool IsUsernamePendingForOtherConnection(
        HSteamNetConnection exceptConn, std::string_view username
    ) const;
    std::vector<std::pair<HSteamNetConnection, std::string>> SnapshotPendingUsernames() const;

    void BindPlayerConnection(PlayerID playerId, HSteamNetConnection conn);
    void UnbindPlayer(PlayerID playerId);
    void ClearPlayerBindings();
    std::optional<HSteamNetConnection> FindConnectionByPlayerId(PlayerID playerId) const;

private:
    std::unordered_map<HSteamNetConnection, ClientSession> m_clients;
    std::unordered_map<HSteamNetConnection, std::string> m_pendingIdentityByConnection;
    std::unordered_map<HSteamNetConnection, std::string> m_pendingUsernameByConnection;
    std::unordered_set<std::string> m_pendingIdentities;
    std::unordered_map<PlayerID, HSteamNetConnection> m_connectionByPlayerId;
};
