#include "ClientNetwork.hpp"

#include <cstring>
#include <vector>

void ClientNetwork::AppendUint32LE(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back((v >> 0) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}

void ClientNetwork::AppendFloatLE(std::vector<uint8_t> &out, float f) {
    uint32_t u;
    static_assert(sizeof(u) == sizeof(f), "float size mismatch");
    std::memcpy(&u, &f, sizeof(u));
    AppendUint32LE(out, u);
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

    std::string effectiveRequestedUsername(requestedUsername);
    if (m_retryWithAutoAssignedUsername && !effectiveRequestedUsername.empty()) {
        std::cout << "[net] retrying connect without requested username after previous rejection\n";
        effectiveRequestedUsername.clear();
        m_retryWithAutoAssignedUsername = false;
    }

    ConnectRequest req;
    req.protocolVersion = kVoxelOpsProtocolVersion;
    req.identity = m_clientIdentity;
    req.requestedUsername = effectiveRequestedUsername;
    const std::vector<uint8_t> out = req.serialize();

    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, out.data(), static_cast<uint32_t>(out.size()), k_nSteamNetworkingSend_Reliable, nullptr
    );
    if (r != k_EResultOK) {
        std::cerr << "SendConnectRequest: SendMessageToConnection failed: " << r << "\n";
        SetConnectionStatus(ConnectionState::Disconnected, "failed to send connect request");
        return false;
    }
    SetConnectionStatus(ConnectionState::Connecting, "waiting for server registration");
    return true;
}

bool ClientNetwork::SendPlayerInput(const PlayerInput &input) {
    if (!IsConnected()) {
        return false;
    }

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

bool ClientNetwork::SendRespawnRequest() {
    if (!IsConnected()) {
        return false;
    }

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

bool ClientNetwork::SendChunkResyncRequest(const glm::ivec3 &chunkPos) {
    if (!IsConnected()) {
        return false;
    }

    std::string payload = "CHUNK_RESYNC|";
    payload += std::to_string(chunkPos.x);
    payload += "|";
    payload += std::to_string(chunkPos.y);
    payload += "|";
    payload += std::to_string(chunkPos.z);

    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(PacketType::Message));
    out.insert(out.end(), payload.begin(), payload.end());

    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        static_cast<uint32_t>(out.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendInventoryActionRequest(const InventoryActionRequest &request) {
    if (!IsConnected()) {
        return false;
    }

    const std::vector<uint8_t> out = request.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        static_cast<uint32_t>(out.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendBlockPlaceRequest(const BlockPlaceRequest &request) {
    if (!IsConnected()) {
        return false;
    }

    const std::vector<uint8_t> out = request.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        static_cast<uint32_t>(out.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendBlockBreakRequest(const BlockBreakRequest &request) {
    if (!IsConnected()) {
        return false;
    }

    const std::vector<uint8_t> out = request.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn,
        out.data(),
        static_cast<uint32_t>(out.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendChunkRequest(const glm::ivec3 &centerChunk, uint16_t viewDistance) {
    if (!IsConnected()) {
        return false;
    }

    ChunkRequest request;
    request.chunkX = centerChunk.x;
    request.chunkY = centerChunk.y;
    request.chunkZ = centerChunk.z;
    request.viewDistance = viewDistance;

    std::vector<uint8_t> out = request.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, out.data(), static_cast<uint32_t>(out.size()), k_nSteamNetworkingSend_Reliable, nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendShootRequest(
    uint32_t clientShotId,
    uint32_t clientTick,
    uint16_t weaponId,
    const glm::vec3 &pos,
    const glm::vec3 &dir,
    uint32_t seed,
    uint8_t inputFlags
) {
    if (!IsConnected()) {
        return false;
    }

    ShootRequest req;
    req.clientShotId = clientShotId;
    req.clientTick = clientTick;
    req.weaponId = weaponId;
    req.posX = pos.x;
    req.posY = pos.y;
    req.posZ = pos.z;
    req.dirX = dir.x;
    req.dirY = dir.y;
    req.dirZ = dir.z;
    req.seed = seed;
    req.inputFlags = inputFlags;

    std::vector<uint8_t> buf = req.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable, nullptr
    );
    return (r == k_EResultOK);
}

bool ClientNetwork::SendGrappleRequest(
    uint32_t clientGrappleId,
    uint32_t clientTick,
    const glm::vec3 &pos,
    const glm::vec3 &dir,
    uint32_t seed
) {
    if (!IsConnected()) {
        return false;
    }

    GrappleRequest req;
    req.clientGrappleId = clientGrappleId;
    req.clientTick = clientTick;
    req.posX = pos.x;
    req.posY = pos.y;
    req.posZ = pos.z;
    req.dirX = dir.x;
    req.dirY = dir.y;
    req.dirZ = dir.z;
    req.seed = seed;

    std::vector<uint8_t> buf = req.serialize();
    const EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        m_conn, buf.data(), static_cast<uint32_t>(buf.size()), k_nSteamNetworkingSend_Reliable, nullptr
    );
    return (r == k_EResultOK);
}
