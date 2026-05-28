#pragma once

#include "../../../Shared/network/PacketType.hpp"
#include "../../../Shared/network/Packets.hpp"
#include "../../engine/world/ChunkManager.hpp"
#include "../../game/player/PlayerManager.hpp"
#include "../persistence/AdminService.hpp"
#include "../persistence/ChatService.hpp"
#include "../session/SessionState.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

#include <functional>
#include <cstddef>
#include <string>
#include <string_view>

class ConnectionService {
public:
    using ClientSession = ClientSessionManager::ClientSession;
    using ChunkCoord = ClientSessionManager::ChunkCoord;

    struct Hooks {
        std::function<std::string(std::string_view, std::string_view, HSteamNetConnection)>
            buildDisplayName;
        std::function<void()> invalidateCombatSnapshotCache;
        std::function<void(PlayerID, size_t)> onSessionAttached;
        std::function<void(const void *, uint32_t, HSteamNetConnection)> broadcastRaw;
        std::function<bool(const ChunkCoord &)> prepareChunkForStreaming;
        std::function<bool(HSteamNetConnection, const ChunkCoord &)> sendChunkData;
        std::function<void(HSteamNetConnection, const ClientSession &, const char *, bool)>
            teardownClientSession;
    };

    ConnectionService(
        SessionState &sessionState,
        PlayerManager &playerManager,
        ChunkManager &chunkManager,
        ChatService &chatService,
        AdminService &adminService,
        HSteamNetPollGroup &pollGroup,
        Hooks hooks
    );

    bool IsInboundRateLimitExceeded(
        HSteamNetConnection incoming, PacketType packetType, uint32_t bytes
    );
    void ReleasePendingRegistration(HSteamNetConnection conn);
    void HandleConnectRequest(HSteamNetConnection incoming, const void *data, uint32_t size);
    void HandleMessagePacket(HSteamNetConnection incoming, const void *data, uint32_t size);
    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo);

private:
    static std::string ReadStringFromPacket(const void *data, uint32_t size, size_t offset = 1);

private:
    SessionState &m_sessionState;
    PlayerManager &m_playerManager;
    ChunkManager &m_chunkManager;
    ChatService &m_chatService;
    AdminService &m_adminService;
    HSteamNetPollGroup &m_pollGroup;
    Hooks m_hooks;
};
