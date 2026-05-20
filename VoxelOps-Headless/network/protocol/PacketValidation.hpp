#pragma once

#include "../../../Shared/network/PacketType.hpp"
#include "../../../Shared/network/Packets.hpp"

#include <cstdint>

namespace NetPacket {

    inline constexpr uint32_t kMaxInboundPacketBytes = 64u * 1024u;
    inline constexpr uint32_t kMaxConnectRequestBytes =
        1u + 2u + 1u + 1u + static_cast<uint32_t>(kMaxConnectIdentityChars) +
        static_cast<uint32_t>(kMaxConnectUsernameChars);
    inline constexpr uint32_t kMaxChatMessageBytes = 1u + 512u;
    inline constexpr uint32_t kPlayerInputPacketBytes = 1u + 4u + 1u + 1u + 2u + 4u * 4u;
    inline constexpr uint32_t kChunkRequestPacketBytes = 1u + 4u + 4u + 4u + 2u;
    inline constexpr uint32_t kBlockPlaceRequestPacketMaxBytes =
        1u + 4u + 2u + static_cast<uint32_t>(kMaxBlockPlaceEditsPerRequest) * (4u + 4u + 4u + 1u);
    inline constexpr uint32_t kBlockBreakRequestPacketMaxBytes =
        1u + 4u + 2u + static_cast<uint32_t>(kMaxBlockBreakEditsPerRequest) * (4u + 4u + 4u);
    inline constexpr uint32_t kShootRequestPacketBytes = 1u + 4u + 4u + 2u + 12u + 12u + 4u + 1u;
    inline constexpr uint32_t kGrappleRequestPacketBytes = 1u + 4u + 4u + 12u + 12u + 4u;
    inline constexpr uint32_t kInventoryActionRequestPacketBytes = 1u + 4u + 4u + 1u + 2u + 2u + 2u;

    bool IsInboundPacketSizeValid(PacketType type, uint32_t bytes);

} // namespace NetPacket
