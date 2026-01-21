#pragma once
#include <cstdint>


enum class PacketType : uint8_t {
    Message = 0,  // optional: text chat, server->client or client->server
    ConnectRequest = 1,  // client -> server (username bytes follow)
    ConnectResponse = 2,  // server -> client (1 byte okFlag)
    ClientConnect = 3,  // server broadcast: new client joined (username)
    ClientDisconnect = 4,  // server broadcast: client left (username)
    // leave a gap for future short-lived messages
    PlayerSnapshot = 9,
    PlayerPosition = 10,  // client -> server: seq + px,py,pz,vx,vy,vz

    ShootRequest = 11,      // client -> server: request to fire
    ShootResult = 12       // server -> client: authoritative shot result
};
