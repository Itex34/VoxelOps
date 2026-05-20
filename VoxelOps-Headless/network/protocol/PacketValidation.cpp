#include "PacketValidation.hpp"

namespace NetPacket {

    bool IsInboundPacketSizeValid(PacketType type, uint32_t bytes) {
        constexpr uint32_t kMinConnectRequestBytes = 1u + 2u + 1u + 1u;
        switch (type) {
        case PacketType::ConnectRequest:
            return bytes >= kMinConnectRequestBytes && bytes <= kMaxConnectRequestBytes;
        case PacketType::Message:
            return bytes >= 1u && bytes <= kMaxChatMessageBytes;
        case PacketType::PlayerInput:
            return bytes == kPlayerInputPacketBytes;
        case PacketType::ChunkRequest:
            return bytes == kChunkRequestPacketBytes;
        case PacketType::BlockPlaceRequest:
            return bytes >= (1u + 4u + 2u) && bytes <= kBlockPlaceRequestPacketMaxBytes;
        case PacketType::BlockBreakRequest:
            return bytes >= (1u + 4u + 2u) && bytes <= kBlockBreakRequestPacketMaxBytes;
        case PacketType::ShootRequest:
            return bytes == kShootRequestPacketBytes;
        case PacketType::GrappleRequest:
            return bytes == kGrappleRequestPacketBytes;
        case PacketType::InventoryActionRequest:
            return bytes == kInventoryActionRequestPacketBytes;
        default:
            return false;
        }
    }

} // namespace NetPacket
