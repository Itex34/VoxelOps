#pragma once
#include <glm/glm.hpp>
#include "../../engine/voxels/Voxel.hpp"

#include <cstdint>

struct GrappleState {
    bool active = false;
    glm::vec3 anchor{0.0f};
    double nextAllowedFireTime = 0.0;
    float ropeLength = 0.0f;
    bool reelingIn = false;
    double lastReelCommandTime = 0.0;
};

struct GrappleFireResult {
    bool accepted = false;
    bool attached = false;
    glm::vec3 anchor{0.0f};
    BlockFace face = BlockFace::PosY;
    uint8_t blockNormal = 255;
};

class ChunkManager;

struct GrappleContext {
    GrappleState &state;
    const glm::vec3 &origin;
    const glm::vec3 &direction;
    const glm::vec3 &playerPosition;
    double nowSeconds;
    const ChunkManager &chunkManager;
};

class GrappleGun {
public:
    GrappleFireResult tryFire(const GrappleContext &ctx);

    void release(GrappleState &state);
    void updatePull(GrappleState &state, glm::vec3 &playerPos, glm::vec3 &playerVel, float dt);

private:
    float m_maxGrappleDistance = 32.0f;
    float m_coolDownSeconds = 0.6f;

};
