#pragma once

#include "RenderScene.hpp"

struct Runtime;

struct RenderSceneBuilderInput {
    bool useDebugCamera = false;
    bool toggleWireframe = false;
    bool toggleChunkBorders = false;
    bool toggleDebugFrustum = false;
    glm::vec3 sunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    float skyExposure = 4.2f;
    glm::vec3 sunShadowDirectionalBias = glm::vec3(0.0f);
    float sunShadowLowSunBiasBoost = 1.0f;
    bool sunShadowFrontFaceCullAtLowSun = false;
    float sunShadowFrontFaceCullGrazingThreshold = 0.78f;
    ImDrawData *uiDrawData = nullptr;
    std::function<void()> renderOpaqueOverlayPasses;
};

class RenderSceneBuilder {
public:
    RenderScene build(Runtime &runtime, const RenderSceneBuilderInput &input) const;
};
