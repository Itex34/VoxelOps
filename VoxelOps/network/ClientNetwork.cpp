#include "ClientNetwork.hpp"

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
        return false;
    }
    m_started = true;
    return true;
}

bool ClientNetwork::ConnectTo(const char ip[16], uint16_t port) {
    if (!m_started.load()) {
        std::cerr << "ClientNetwork: Start() must be called first\n";
        return false;
    }

    // parse dotted IPv4 like "127.0.0.1"
    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        std::cerr << "ConnectTo: invalid ip string\n";
        return false;
    }
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        std::cerr << "ConnectTo: ip octet out of range\n";
        return false;
    }

    SteamNetworkingIPAddr addr;
    addr.Clear();

    // Build 32-bit IPv4 address in network byte order: 0xAABBCCDD for A.B.C.D
    uint32_t ipNum =
        ((uint32_t)(a & 0xFF) << 24) |
        ((uint32_t)(b & 0xFF) << 16) |
        ((uint32_t)(c & 0xFF) << 8) |
        ((uint32_t)(d & 0xFF));



    // Use the SetIPv4(ip, port) overload (user indicated this is the available signature)
    addr.SetIPv4(ipNum, port);

    // If SetIPv4 does not set the port (signature differs), set it explicitly:
    addr.m_port = port;

    // connect (no extra options)
    m_conn = SteamNetworkingSockets()->ConnectByIPAddress(addr, 0, nullptr);
    if (m_conn == k_HSteamNetConnection_Invalid) {
        std::cerr << "ConnectByIPAddress failed\n";
        return false;
    }
    return true;
}

bool ClientNetwork::SendConnectRequest() {
    if (m_conn == k_HSteamNetConnection_Invalid) {
        std::cerr << "SendConnectRequest: no connection\n";
        return false;
    }
    std::vector<uint8_t> out{ static_cast<uint8_t>(PacketType::ConnectRequest) };
    EResult r = SteamNetworkingSockets()->SendMessageToConnection(m_conn, out.data(), (uint32_t)out.size(), k_nSteamNetworkingSend_Reliable, nullptr);
    if (r != k_EResultOK) {
        std::cerr << "SendConnectRequest: SendMessageToConnection failed: " << r << "\n";
        return false;
    }
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

bool ClientNetwork::SendChunkRequest(const glm::ivec3& centerChunk, uint16_t viewDistance)
{
    if (m_conn == k_HSteamNetConnection_Invalid) return false;

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

    // run callbacks (connection state, etc.)
    SteamNetworkingSockets()->RunCallbacks();

    // receive messages on the connection (drain)
    if (m_conn == k_HSteamNetConnection_Invalid) return;
    SteamNetworkingMessage_t* pMsg = nullptr;
    while (SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_conn, &pMsg, 1) > 0 && pMsg) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(pMsg->m_pData);
        uint32_t cb = pMsg->m_cbSize;
        if (cb >= 1) {
            OnMessage(data, cb);
        }
        pMsg->Release();
    }
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

    {
        std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
        m_chunkDataQueue.clear();
        m_chunkDeltaQueue.clear();
        m_chunkUnloadQueue.clear();
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
        if (size >= 2) {
            uint8_t ok = data[1];
            if (ok) {
                m_registered = true;
                std::cout << "[net] registered by server\n";
            }
            else {
                std::cout << "[net] registration rejected by server\n";
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::Message) {
        // simple text message from server
        if (size > 1) {
            std::string s(reinterpret_cast<const char*>(data + 1), size - 1);
            std::cout << "[server msg] " << s << "\n";
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkData) {
        std::vector<uint8_t> buf(data, data + size);
        auto opt = ChunkData::deserialize(buf);
        if (!opt.has_value()) {
            std::cerr << "[net] malformed ChunkData\n";
            return;
        }

        ChunkData packet = std::move(*opt);
        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_chunkDataQueue.push_back(packet);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkDelta) {
        std::vector<uint8_t> buf(data, data + size);
        auto opt = ChunkDelta::deserialize(buf);
        if (!opt.has_value()) {
            std::cerr << "[net] malformed ChunkDelta\n";
            return;
        }

        ChunkDelta packet = std::move(*opt);
        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_chunkDeltaQueue.push_back(packet);
        }

        ChunkAck ack;
        ack.ackedType = static_cast<uint8_t>(PacketType::ChunkDelta);
        ack.chunkX = packet.chunkX;
        ack.chunkY = packet.chunkY;
        ack.chunkZ = packet.chunkZ;
        ack.version = packet.resultingVersion;
        const std::vector<uint8_t> ackBuf = ack.serialize();
        SteamNetworkingSockets()->SendMessageToConnection(m_conn, ackBuf.data(), (uint32_t)ackBuf.size(), k_nSteamNetworkingSend_Reliable, nullptr);
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkUnload) {
        std::vector<uint8_t> buf(data, data + size);
        auto opt = ChunkUnload::deserialize(buf);
        if (!opt.has_value()) {
            std::cerr << "[net] malformed ChunkUnload\n";
            return;
        }

        ChunkUnload packet = std::move(*opt);
        {
            std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
            m_chunkUnloadQueue.push_back(packet);
        }

        ChunkAck ack;
        ack.ackedType = static_cast<uint8_t>(PacketType::ChunkUnload);
        ack.chunkX = packet.chunkX;
        ack.chunkY = packet.chunkY;
        ack.chunkZ = packet.chunkZ;
        ack.version = 0;
        const std::vector<uint8_t> ackBuf = ack.serialize();
        SteamNetworkingSockets()->SendMessageToConnection(m_conn, ackBuf.data(), (uint32_t)ackBuf.size(), k_nSteamNetworkingSend_Reliable, nullptr);
        return;
    }

    // handle ShootResult (server -> client authoritative shot result)
    if (static_cast<PacketType>(t) == PacketType::ShootResult) {
        // copy incoming bytes into vector for deserialization helper
        std::vector<uint8_t> buf(data, data + size);
        auto opt = ShootResult::deserialize(buf);
        if (!opt.has_value()) {
            std::cerr << "[net] malformed ShootResult\n";
            return;
        }
        ShootResult res = *opt;

        // Example handling: match by clientShotId, reconcile ammo, spawn VFX
        // NOTE: replace the prints with your in-game handlers
        if (!res.accepted) {
            std::cout << "[shoot] server rejected shot id=" << res.clientShotId << "\n";
            // Reconcile: restore ammo/prediction as needed
        }
        else {
            if (res.didHit) {
                std::cout << "[shoot] hit entity=" << res.hitEntityId
                    << " at (" << res.hitX << "," << res.hitY << "," << res.hitZ << ") dmg=" << res.damageApplied << "\n";
                // spawn impact VFX at (res.hitX,res.hitY,res.hitZ), show damage numbers, etc.
            }
            else {
                std::cout << "[shoot] miss endpoint (" << res.hitX << "," << res.hitY << "," << res.hitZ << ")\n";
                // show tracer to endpoint if you predicted a tracer client-side
            }

            // Authoritative ammo reconciliation:
            // update your local player/gun state so it matches server
            // e.g. player->GetGun().SetAmmo(res.newAmmoCount);
            // or queue an in-game event to set ammo to res.newAmmoCount
            std::cout << "[shoot] server ammo=" << res.newAmmoCount << "\n";
        }
        return;
    }
}



bool ClientNetwork::SendShootRequest(uint32_t clientShotId, uint32_t clientTick, uint16_t weaponId,
    const glm::vec3& pos, const glm::vec3& dir,
    uint32_t seed, uint8_t inputFlags)
{
    if (m_conn == k_HSteamNetConnection_Invalid) return false;

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

ClientNetwork::ChunkQueueDepths ClientNetwork::GetChunkQueueDepths()
{
    std::lock_guard<std::mutex> lk(m_chunkQueueMutex);
    ChunkQueueDepths depths;
    depths.chunkData = m_chunkDataQueue.size();
    depths.chunkDelta = m_chunkDeltaQueue.size();
    depths.chunkUnload = m_chunkUnloadQueue.size();
    return depths;
}

