#pragma once
#include <cstdint>


enum class PacketType : uint8_t {
    Message = 0,  // optional: text chat, server->client or client->server
    ConnectRequest = 1,  // client -> server: protocolVersion + identity + requested username
    ConnectResponse = 2,  // server -> client: okFlag + reject reason + protocolVersion + assigned username
    ClientConnect = 3,  // server broadcast: new client joined (username)
    ClientDisconnect = 4,  // server broadcast: client left (username)

    PlayerInput = 8,      // client -> server: seq + movement input
    PlayerSnapshot = 9,
    PlayerPosition = 10,  // client -> server: seq + px,py,pz,vx,vy,vz

    ShootRequest = 11,      // client -> server: request to fire
    ShootResult = 12,       // server -> client: authoritative shot result

    ChunkRequest = 20,      // client -> server: request/refresh chunk interest area
    ChunkData = 21,         // server -> client: full chunk payload (serialized/compressed bytes)
    ChunkDelta = 22,        // server -> client: block edits for a chunk
    ChunkUnload = 23,       // server -> client: unload one chunk

    InventoryActionRequest = 24, // client -> server: inventory command request
    InventoryActionResult = 25,  // server -> client: command accept/reject + revision
    InventorySnapshot = 26,      // server -> client: authoritative inventory state

    WorldItemSnapshot = 27       // server -> client: authoritative nearby dropped items
};
