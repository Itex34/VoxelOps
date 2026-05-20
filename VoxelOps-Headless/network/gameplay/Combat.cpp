#include "../core/Runtime.hpp"
#include "../protocol/PacketParsers.hpp"

#include <iostream>
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

} // namespace

void Runtime::HandleShootRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    ShootRequest req{};
    if (!NetPacket::ParseShootRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cerr << "[recv] malformed ShootRequest\n";
        return;
    }

    const ShootResult result = ExecuteShootRequest(incoming, req);
    SendShootResult(incoming, result);
}

void Runtime::HandleGrappleRequestPacket(
    HSteamNetConnection incoming, const void *data, uint32_t size
) {
    GrappleRequest req{};
    if (!NetPacket::ParseGrappleRequestPacket(reinterpret_cast<const uint8_t *>(data), size, req)) {
        std::cerr << "[recv] malformed GrappleRequest\n";
        return;
    }

    const GrappleResult result = ExecuteGrappleRequest(incoming, req);
    SendGrappleResult(incoming, result);
}
