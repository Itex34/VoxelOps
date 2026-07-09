#include "CombatRequestService.hpp"

#include "../core/DiagnosticsFlags.hpp"
#include "../protocol/PacketParsers.hpp"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <utility>
#include <vector>

namespace {

void SendShootResult(HSteamNetConnection incoming, const ShootResult &result) {
    const std::vector<uint8_t> outBuf = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming,
        outBuf.data(),
        static_cast<uint32_t>(outBuf.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

void SendGrappleResult(HSteamNetConnection incoming, const GrappleResult &result) {
    const std::vector<uint8_t> outBuf = result.serialize();
    (void)SteamNetworkingSockets()->SendMessageToConnection(
        incoming,
        outBuf.data(),
        static_cast<uint32_t>(outBuf.size()),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

std::atomic<uint64_t> g_slowShootRequestCount{0};

} // namespace

CombatRequestService::CombatRequestService(Hooks hooks)
    : m_hooks(std::move(hooks)) {}

void CombatRequestService::HandleShootRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    ShootRequest req{};
    if (!NetPacket::ParseShootRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cerr << "[recv] malformed ShootRequest\n";
        return;
    }

    const auto shootStart = std::chrono::steady_clock::now();
    const ShootResult result = m_hooks.executeShootRequest(incoming, req);
    const auto shootUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - shootStart
    )
                             .count();
    if (DiagnosticsFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire) &&
        shootUs >= 2000) {
        const uint64_t count = g_slowShootRequestCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 40 || (count % 200) == 0) {
            std::cerr << "[perf/shoot] slow request us=" << shootUs
                      << " conn=" << incoming
                      << " shotId=" << req.clientShotId
                      << " count=" << count << "\n";
        }
    }
    SendShootResult(incoming, result);
}

void CombatRequestService::HandleGrappleRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    GrappleRequest req{};
    if (!NetPacket::ParseGrappleRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cerr << "[recv] malformed GrappleRequest\n";
        return;
    }

    const GrappleResult result = m_hooks.executeGrappleRequest(incoming, req);
    SendGrappleResult(incoming, result);
}
