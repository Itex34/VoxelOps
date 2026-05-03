#pragma once

#include "../../../Shared/network/Packets.hpp"

#include <cstdint>

namespace NetPacket {

    bool ParsePlayerInputPacket(const uint8_t *data, uint32_t size, PlayerInput &out);
    bool ParseChunkRequestPacket(const uint8_t *data, uint32_t size, ChunkRequest &out);
    bool ParseShootRequestPacket(const uint8_t *data, uint32_t size, ShootRequest &out);
    bool ParseInventoryActionRequestPacket(
        const uint8_t *data, uint32_t size, InventoryActionRequest &out
    );
    bool ParseBlockPlaceRequestPacket(const uint8_t *data, uint32_t size, BlockPlaceRequest &out);
    bool ParseBlockBreakRequestPacket(const uint8_t *data, uint32_t size, BlockBreakRequest &out);

} // namespace NetPacket
