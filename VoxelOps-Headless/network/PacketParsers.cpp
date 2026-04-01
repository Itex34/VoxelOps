#include "PacketParsers.hpp"

#include "PacketReader.hpp"
#include "PacketValidation.hpp"

#include <cmath>
#include <optional>
#include <vector>

namespace NetPacket {

bool ParsePlayerInputPacket(const uint8_t* data, uint32_t size, PlayerInput& out)
{
    if (!data || size != kPlayerInputPacketBytes) {
        return false;
    }
    out.inputTick = ReadU32LE(data + 1);
    out.inputFlags = data[5];
    out.flyMode = data[6];
    out.weaponId = ReadU16LE(data + 7);
    out.yaw = ReadF32LE(data + 9);
    out.pitch = ReadF32LE(data + 13);
    out.moveX = ReadF32LE(data + 17);
    out.moveZ = ReadF32LE(data + 21);
    return std::isfinite(out.yaw) &&
        std::isfinite(out.pitch) &&
        std::isfinite(out.moveX) &&
        std::isfinite(out.moveZ);
}

bool ParseChunkRequestPacket(const uint8_t* data, uint32_t size, ChunkRequest& out)
{
    if (!data || size != kChunkRequestPacketBytes) {
        return false;
    }
    out.chunkX = ReadI32LE(data + 1);
    out.chunkY = ReadI32LE(data + 5);
    out.chunkZ = ReadI32LE(data + 9);
    out.viewDistance = ReadU16LE(data + 13);
    return true;
}

bool ParseShootRequestPacket(const uint8_t* data, uint32_t size, ShootRequest& out)
{
    if (!data || size != kShootRequestPacketBytes) {
        return false;
    }
    out.clientShotId = ReadU32LE(data + 1);
    out.clientTick = ReadU32LE(data + 5);
    out.weaponId = ReadU16LE(data + 9);
    out.posX = ReadF32LE(data + 11);
    out.posY = ReadF32LE(data + 15);
    out.posZ = ReadF32LE(data + 19);
    out.dirX = ReadF32LE(data + 23);
    out.dirY = ReadF32LE(data + 27);
    out.dirZ = ReadF32LE(data + 31);
    out.seed = ReadU32LE(data + 35);
    out.inputFlags = data[39];
    return
        std::isfinite(out.posX) &&
        std::isfinite(out.posY) &&
        std::isfinite(out.posZ) &&
        std::isfinite(out.dirX) &&
        std::isfinite(out.dirY) &&
        std::isfinite(out.dirZ);
}

bool ParseInventoryActionRequestPacket(const uint8_t* data, uint32_t size, InventoryActionRequest& out)
{
    if (!data || size != kInventoryActionRequestPacketBytes) {
        return false;
    }

    std::vector<uint8_t> bytes(data, data + size);
    const std::optional<InventoryActionRequest> parsed = InventoryActionRequest::deserialize(bytes);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ParseBlockPlaceRequestPacket(const uint8_t* data, uint32_t size, BlockPlaceRequest& out)
{
    if (!data || size < (1u + 4u + 2u) || size > kBlockPlaceRequestPacketMaxBytes) {
        return false;
    }

    std::vector<uint8_t> bytes(data, data + size);
    const std::optional<BlockPlaceRequest> parsed = BlockPlaceRequest::deserialize(bytes);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ParseBlockBreakRequestPacket(const uint8_t* data, uint32_t size, BlockBreakRequest& out)
{
    if (!data || size < (1u + 4u + 2u) || size > kBlockBreakRequestPacketMaxBytes) {
        return false;
    }

    std::vector<uint8_t> bytes(data, data + size);
    const std::optional<BlockBreakRequest> parsed = BlockBreakRequest::deserialize(bytes);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

} // namespace NetPacket

