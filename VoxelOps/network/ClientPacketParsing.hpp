#pragma once

#include <cstdint>
#include <span>

#include "../../Shared/network/Packets.hpp"

namespace ClientPackets {

bool ParseConnectResponse(std::span<const uint8_t> bytes, ConnectResponse &out);
bool ParsePlayerSnapshotFrame(std::span<const uint8_t> bytes, PlayerSnapshotFrame &out);
bool ParseChunkData(std::span<const uint8_t> bytes, ChunkData &out);
bool ParseChunkDelta(std::span<const uint8_t> bytes, ChunkDelta &out);
bool ParseChunkUnload(std::span<const uint8_t> bytes, ChunkUnload &out);
bool ParseShootResult(std::span<const uint8_t> bytes, ShootResult &out);
bool ParseGrappleResult(std::span<const uint8_t> bytes, GrappleResult &out);
bool ParseInventoryActionResult(std::span<const uint8_t> bytes, InventoryActionResult &out);
bool ParseInventorySnapshot(std::span<const uint8_t> bytes, InventorySnapshot &out);
bool ParseWorldItemSnapshot(std::span<const uint8_t> bytes, WorldItemSnapshot &out);
bool ParseBlockPlaceResult(std::span<const uint8_t> bytes, BlockPlaceResult &out);
bool ParseBlockBreakResult(std::span<const uint8_t> bytes, BlockBreakResult &out);

} // namespace ClientPackets
