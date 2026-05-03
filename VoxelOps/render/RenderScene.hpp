#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ChunkMeshData.hpp"
#include "../voxels/Chunk.hpp"
#include "../voxels/VoxelCoordHash.hpp"

class Camera;
struct ImDrawData;

struct RenderRemotePlayerState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    uint16_t weaponId = 0;
};

struct RenderChunkWorldView {
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> *cpuChunkMeshes = nullptr;
    const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> *chunks = nullptr;
    bool enableAO = true;
};

struct RenderScene {
    RenderChunkWorldView chunkWorld;
    const Camera &activeCamera;
    const Camera *cullingCamera = nullptr;
    glm::vec3 localPlayerPosition{0.0f};
    uint16_t chunkRenderDistance = 12;
    std::vector<RenderRemotePlayerState> remotePlayers;
    glm::vec3 sunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    float skyExposure = 4.2f;
    bool toggleWireframe = false;
    bool toggleChunkBorders = false;
    bool toggleDebugFrustum = false;
    glm::vec3 sunShadowDirectionalBias = glm::vec3(0.0f); // x=+Y, y=side, z=-Y
    float sunShadowLowSunBiasBoost = 1.0f;
    bool sunShadowFrontFaceCullAtLowSun = false;
    float sunShadowFrontFaceCullGrazingThreshold = 0.78f;
    ImDrawData *uiDrawData = nullptr;
    std::function<void()> renderOpaqueOverlayPasses;
    bool useDebugCamera = false;
};
