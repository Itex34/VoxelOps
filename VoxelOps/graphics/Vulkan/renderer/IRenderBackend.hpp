#pragma once

#include <glm/glm.hpp>

#include <cstdint>

#include "graphics/Vulkan/renderer/RenderFrameData.hpp"

class IRenderBackend {
  public:
    virtual ~IRenderBackend() = default;

    virtual void init() = 0;
    virtual void renderFrame(uint32_t windowWidth, uint32_t windowHeight,
                             const glm::mat4 &viewMatrix, const glm::mat4 &projectionMatrix,
                             const glm::mat4 &viewProjection, const FrameRenderData &frameData) = 0;
    virtual void handleWindowResize(uint32_t windowWidth, uint32_t windowHeight) = 0;
    virtual void cleanup() = 0;
};
