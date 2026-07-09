#include "ClientNetwork.hpp"

#include "ClientMessageParsing.hpp"
#include "ClientPacketParsing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <span>
#include <utility>

namespace {

    constexpr size_t kMaxChunkDataQueueDepth = 4096;
    constexpr size_t kMaxChunkDeltaQueueDepth = 512;
    constexpr size_t kMaxChunkUnloadQueueDepth = 256;
    constexpr size_t kMaxKillFeedQueueDepth = 64;
    constexpr size_t kMaxScoreboardQueueDepth = 16;
    constexpr size_t kMaxWorldItemSnapshotQueueDepth = 8;
    constexpr size_t kMaxBlockPlaceResultQueueDepth = 128;
    constexpr size_t kMaxBlockBreakResultQueueDepth = 128;
    constexpr size_t kMaxGrappleResultQueueDepth = 32;
    constexpr size_t kMaxMessagesPerPoll = 128;
    constexpr int64_t kMessagePollBudgetUs = 2000;
    constexpr bool kEnableClientNetProfiling = false;

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

    ClientNetProfileState &GetClientNetProfileState() {
        static ClientNetProfileState state;
        return state;
    }

    void RecordClientNetProfile(
        ClientNetwork *net,
        int64_t pollUs,
        int64_t callbackUs,
        int64_t recvUs,
        uint64_t messages,
        uint64_t bytes
    ) {
        if (!kEnableClientNetProfiling || net == nullptr) {
            return;
        }

        ClientNetProfileState &state = GetClientNetProfileState();
        state.polls += 1;
        state.messages += messages;
        state.bytes += bytes;
        state.totalPollUs += pollUs;
        state.totalCallbackUs += callbackUs;
        state.totalRecvUs += recvUs;
        state.maxPollUs = std::max(state.maxPollUs, pollUs);

        const auto now = std::chrono::steady_clock::now();
        const double elapsedSec =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - state.lastLog).count();
        if (elapsedSec < 1.0) {
            return;
        }

        const double polls = (state.polls > 0) ? static_cast<double>(state.polls) : 1.0;
        const double msgs = static_cast<double>(state.messages);
        const double avgPollMs = static_cast<double>(state.totalPollUs) / (polls * 1000.0);
        const double avgCallbackMs = static_cast<double>(state.totalCallbackUs) / (polls * 1000.0);
        const double avgRecvMs = static_cast<double>(state.totalRecvUs) / (polls * 1000.0);
        const double maxPollMs = static_cast<double>(state.maxPollUs) / 1000.0;
        const double avgMsgUs =
            (state.messages > 0) ? (static_cast<double>(state.totalRecvUs) / msgs) : 0.0;

        const ClientNetwork::ChunkQueueDepths queueDepths = net->GetChunkQueueDepths();
        std::cerr << "[net/profile] polls=" << state.polls << " msgs=" << state.messages
                  << " bytes=" << state.bytes << " pollAvgMs=" << std::fixed << std::setprecision(3)
                  << avgPollMs << " pollMaxMs=" << maxPollMs << " cbAvgMs=" << avgCallbackMs
                  << " recvAvgMs=" << avgRecvMs << " msgAvgUs=" << avgMsgUs
                  << " queue(data/delta/unload)=(" << queueDepths.chunkData << "/"
                  << queueDepths.chunkDelta << "/" << queueDepths.chunkUnload << ")\n";

        state.lastLog = now;
        state.polls = 0;
        state.messages = 0;
        state.bytes = 0;
        state.totalPollUs = 0;
        state.totalCallbackUs = 0;
        state.totalRecvUs = 0;
        state.maxPollUs = 0;
    }

    template <typename T> void TrimQueueToDepth(std::deque<T> &queue, size_t maxDepth) {
        while (queue.size() > maxDepth) {
            queue.pop_front();
        }
    }

} // namespace

void ClientNetwork::Poll() {
    if (!m_started.load()) {
        return;
    }

    const auto pollTotalStart = std::chrono::steady_clock::now();

    SteamNetworkingSockets()->RunCallbacks();
    const auto afterCallbacks = std::chrono::steady_clock::now();
    const int64_t callbackUs =
        std::chrono::duration_cast<std::chrono::microseconds>(afterCallbacks - pollTotalStart)
            .count();

    if (m_conn == k_HSteamNetConnection_Invalid) {
        const auto pollTotalEnd = std::chrono::steady_clock::now();
        const int64_t pollUs =
            std::chrono::duration_cast<std::chrono::microseconds>(pollTotalEnd - pollTotalStart)
                .count();
        RecordClientNetProfile(this, pollUs, callbackUs, 0, 0, 0);
        return;
    }
    SteamNetConnectionInfo_t info{};
    if (SteamNetworkingSockets()->GetConnectionInfo(m_conn, &info)) {
        if (info.m_eState == k_ESteamNetworkingConnectionState_Connected && m_registered) {
            if (m_assignedUsername.empty()) {
                SetConnectionStatus(ConnectionState::Connected, "connected");
            } else {
                SetConnectionStatus(
                    ConnectionState::Connected, std::string("connected as ") + m_assignedUsername
                );
            }
        } else if (info.m_eState == k_ESteamNetworkingConnectionState_Connecting && !m_registered) {
            if (m_connectionState != ConnectionState::Connecting) {
                SetConnectionStatus(ConnectionState::Connecting, "connecting");
            }
        } else if (
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
            m_chunkResyncOverflowCooldownUntil.clear();
            SetConnectionStatus(ConnectionState::Disconnected, reason);
            const auto pollTotalEnd = std::chrono::steady_clock::now();
            const int64_t pollUs =
                std::chrono::duration_cast<std::chrono::microseconds>(pollTotalEnd - pollTotalStart)
                    .count();
            RecordClientNetProfile(this, pollUs, callbackUs, 0, 0, 0);
            return;
        }
    }

    const auto pollBudgetStart = std::chrono::steady_clock::now();
    size_t drainedMessages = 0;
    uint64_t drainedBytes = 0;
    SteamNetworkingMessage_t *pMsg = nullptr;
    while (drainedMessages < kMaxMessagesPerPoll &&
           SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_conn, &pMsg, 1) > 0 && pMsg) {
        const uint8_t *data = reinterpret_cast<const uint8_t *>(pMsg->m_pData);
        const uint32_t cb = pMsg->m_cbSize;

        if (cb >= 1) {
            const PacketType type = static_cast<PacketType>(data[0]);
            const bool highPriority = (type == PacketType::PlayerSnapshot) ||
                                      (type == PacketType::ShootResult) ||
                                      (type == PacketType::GrappleResult);

            OnMessage(data, cb);

            drainedBytes += cb;
            ++drainedMessages;

            if (!highPriority) {
                const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - pollBudgetStart
                )
                                              .count();

                if (elapsedUs >= kMessagePollBudgetUs) {
                    break;
                }
            }
        }

        pMsg->Release();
    }

    const auto pollTotalEnd = std::chrono::steady_clock::now();
    const int64_t pollUs =
        std::chrono::duration_cast<std::chrono::microseconds>(pollTotalEnd - pollTotalStart)
            .count();
    const int64_t recvUs =
        std::chrono::duration_cast<std::chrono::microseconds>(pollTotalEnd - pollBudgetStart)
            .count();
    RecordClientNetProfile(this, pollUs, callbackUs, recvUs, drainedMessages, drainedBytes);
}

void ClientNetwork::OnMessage(const uint8_t *data, uint32_t size) {
    const uint8_t t = data[0];
    if (static_cast<PacketType>(t) == PacketType::ConnectResponse) {
        ConnectResponse resp;
        if (!ClientPackets::ParseConnectResponse(std::span<const uint8_t>(data, size), resp)) {
            std::cerr << "[net] malformed ConnectResponse\n";
            m_registered = false;
            m_assignedUsername.clear();
            SetConnectionStatus(ConnectionState::Disconnected, "malformed connect response", false);
            if (m_conn != k_HSteamNetConnection_Invalid) {
                SteamNetworkingSockets()->CloseConnection(
                    m_conn, 0, "malformed connect response", false
                );
                m_conn = k_HSteamNetConnection_Invalid;
            }
            return;
        }

        if (resp.ok != 0) {
            m_registered = true;
            m_hasEverConnectedSuccessfully = true;
            m_retryWithAutoAssignedUsername = false;
            m_assignedUsername = resp.assignedUsername;
            const std::string displayName = m_assignedUsername.empty()
                                                ? std::string("connected")
                                                : ("connected as " + m_assignedUsername);
            SetConnectionStatus(ConnectionState::Connected, displayName);
            std::cout << "[net] registered by server";
            if (!m_assignedUsername.empty()) {
                std::cout << " as " << m_assignedUsername;
            }
            std::cout << "\n";
        } else {
            m_registered = false;
            m_assignedUsername.clear();
            std::string reason =
                resp.message.empty() ? std::string("registration rejected") : resp.message;
            std::cout << "[net] registration rejected by server: " << reason << "\n";

            if (resp.reason == ConnectRejectReason::IdentityInUse) {
                m_useTransientIdentity = true;
                m_clientIdentity.clear();
                std::cout << "[net] identity conflict detected; rotating to transient identity for "
                             "retry\n";
            }
            if (resp.reason == ConnectRejectReason::UsernameTaken &&
                m_hasEverConnectedSuccessfully) {
                m_retryWithAutoAssignedUsername = true;
                std::cout << "[net] username rejected during reconnect; will retry with "
                             "server-assigned username\n";
            }

            const bool fatalMismatch = (resp.reason == ConnectRejectReason::ProtocolMismatch) ||
                                       (resp.reason == ConnectRejectReason::InvalidIdentity);
            bool allowReconnect = !fatalMismatch;
            if (resp.reason == ConnectRejectReason::UsernameTaken &&
                !m_hasEverConnectedSuccessfully) {
                allowReconnect = false;
            }
            SetConnectionStatus(ConnectionState::Disconnected, reason, allowReconnect);

            if (m_conn != k_HSteamNetConnection_Invalid) {
                SteamNetworkingSockets()->CloseConnection(m_conn, 0, reason.c_str(), false);
                m_conn = k_HSteamNetConnection_Invalid;
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::Message) {
        if (size > 1) {
            std::string s(reinterpret_cast<const char *>(data + 1), size - 1);
            if (s == "server_heartbeat") {
                return;
            }
            KillFeedEvent killEvent;
            if (ClientMessages::TryParseKillFeedMessage(s, killEvent)) {
                std::lock_guard<std::mutex> lk(m_inboundMutex);
                m_killFeedQueue.push_back(std::move(killEvent));
                TrimQueueToDepth(m_killFeedQueue, kMaxKillFeedQueueDepth);
                return;
            }

            ScoreboardSnapshot scoreboardSnapshot;
            if (ClientMessages::TryParseScoreboardMessage(s, scoreboardSnapshot)) {
                std::lock_guard<std::mutex> lk(m_inboundMutex);
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
        if (!ClientPackets::ParsePlayerSnapshotFrame(std::span<const uint8_t>(data, size), frame)) {
            std::cerr << "[net] malformed PlayerSnapshot\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_playerSnapshotQueue.push_back(std::move(frame));
            while (m_playerSnapshotQueue.size() > 8) {
                m_playerSnapshotQueue.pop_front();
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkData) {
        ChunkData packet;
        if (!ClientPackets::ParseChunkData(std::span<const uint8_t>(data, size), packet)) {
            std::cerr << "[net] malformed ChunkData\n";
            return;
        }

        bool droppedChunkData = false;
        glm::ivec3 droppedChunkPos(0);
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            if (m_chunkDataQueue.size() >= kMaxChunkDataQueueDepth) {
                const ChunkData &dropped = m_chunkDataQueue.front();
                droppedChunkPos = glm::ivec3(dropped.chunkX, dropped.chunkY, dropped.chunkZ);
                m_chunkDataQueue.pop_front();
                droppedChunkData = true;
            }
            m_chunkDataQueue.push_back(std::move(packet));
        }

        if (droppedChunkData) {
            static uint64_t s_droppedChunkDataCount = 0;
            ++s_droppedChunkDataCount;
            if (s_droppedChunkDataCount <= 20 || (s_droppedChunkDataCount % 100) == 0) {
                std::cerr << "[net] chunk data queue overflow, resync requested chunk=("
                          << droppedChunkPos.x << "," << droppedChunkPos.y << ","
                          << droppedChunkPos.z << ")"
                          << " count=" << s_droppedChunkDataCount << "\n";
            }
            PruneChunkResyncOverflowState();
            if (ShouldSendChunkResyncForOverflow(droppedChunkPos)) {
                (void)SendChunkResyncRequest(droppedChunkPos);
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkDelta) {
        ChunkDelta packet;
        if (!ClientPackets::ParseChunkDelta(std::span<const uint8_t>(data, size), packet)) {
            std::cerr << "[net] malformed ChunkDelta\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_chunkDeltaQueue.push_back(std::move(packet));
            TrimQueueToDepth(m_chunkDeltaQueue, kMaxChunkDeltaQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ChunkUnload) {
        ChunkUnload packet;
        if (!ClientPackets::ParseChunkUnload(std::span<const uint8_t>(data, size), packet)) {
            std::cerr << "[net] malformed ChunkUnload\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_chunkUnloadQueue.push_back(std::move(packet));
            TrimQueueToDepth(m_chunkUnloadQueue, kMaxChunkUnloadQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::ShootResult) {
        ShootResult result;
        if (!ClientPackets::ParseShootResult(std::span<const uint8_t>(data, size), result)) {
            std::cerr << "[net] malformed ShootResult\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_shootResultQueue.push_back(std::move(result));
            while (m_shootResultQueue.size() > 32) {
                m_shootResultQueue.pop_front();
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::GrappleResult) {
        GrappleResult result;
        if (!ClientPackets::ParseGrappleResult(std::span<const uint8_t>(data, size), result)) {
            std::cerr << "[net] malformed GrappleResult\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_grappleResultQueue.push_back(std::move(result));
            TrimQueueToDepth(m_grappleResultQueue, kMaxGrappleResultQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::InventoryActionResult) {
        InventoryActionResult result;
        if (!ClientPackets::ParseInventoryActionResult(
                std::span<const uint8_t>(data, size), result
            )) {
            std::cerr << "[net] malformed InventoryActionResult\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_inventoryActionResultQueue.push_back(std::move(result));
            while (m_inventoryActionResultQueue.size() > 64) {
                m_inventoryActionResultQueue.pop_front();
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::InventorySnapshot) {
        InventorySnapshot snapshot;
        if (!ClientPackets::ParseInventorySnapshot(
                std::span<const uint8_t>(data, size), snapshot
            )) {
            std::cerr << "[net] malformed InventorySnapshot\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_inventorySnapshotQueue.push_back(std::move(snapshot));
            while (m_inventorySnapshotQueue.size() > 8) {
                m_inventorySnapshotQueue.pop_front();
            }
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::WorldItemSnapshot) {
        WorldItemSnapshot snapshot;
        if (!ClientPackets::ParseWorldItemSnapshot(
                std::span<const uint8_t>(data, size), snapshot
            )) {
            std::cerr << "[net] malformed WorldItemSnapshot\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_worldItemSnapshotQueue.push_back(std::move(snapshot));
            TrimQueueToDepth(m_worldItemSnapshotQueue, kMaxWorldItemSnapshotQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::BlockPlaceResult) {
        BlockPlaceResult result;
        if (!ClientPackets::ParseBlockPlaceResult(std::span<const uint8_t>(data, size), result)) {
            std::cerr << "[net] malformed BlockPlaceResult\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_blockPlaceResultQueue.push_back(std::move(result));
            TrimQueueToDepth(m_blockPlaceResultQueue, kMaxBlockPlaceResultQueueDepth);
        }
        return;
    }

    if (static_cast<PacketType>(t) == PacketType::BlockBreakResult) {
        BlockBreakResult result;
        if (!ClientPackets::ParseBlockBreakResult(std::span<const uint8_t>(data, size), result)) {
            std::cerr << "[net] malformed BlockBreakResult\n";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_inboundMutex);
            m_blockBreakResultQueue.push_back(std::move(result));
            TrimQueueToDepth(m_blockBreakResultQueue, kMaxBlockBreakResultQueueDepth);
        }
        return;
    }
}
