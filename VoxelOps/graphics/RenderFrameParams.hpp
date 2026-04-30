#pragma once

#include <functional>

#include <glm/vec3.hpp>

class Shader;
class ChunkManager;
class Frustum;
class Player;
class Camera;
struct ImDrawData;

struct RenderFrameParams {
    Shader *debugShader = nullptr;
    ChunkManager &chunkManager;
    Frustum &frustum;
    Player &player;
    const Camera &activeCamera;
    const Camera *cullingCamera = nullptr;
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
};
