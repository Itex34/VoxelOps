#pragma once

#include "renderer/RenderFrameData.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

class VulkanGiSettings {
  public:
    struct FrameDecision {
        GiTracingBackend backend = GiTracingBackend::SoftwareDda;
        bool hardwareRtSupported = false;
        bool rtSceneReady = false;
        bool resetHistory = false;
        glm::vec3 sunDirection = glm::vec3(0.25f, 0.85f, 0.42f);
    };

    FrameDecision beginFrame(const glm::vec3 &cameraPosition, const glm::vec3 &sunDirection,
                             bool hardwareRtSupported, VkAccelerationStructureKHR sceneTlas,
                             int tracingBackendPreference);

    void fillLightingData(GiLightingData &lighting, const FrameDecision &decision,
                          uint32_t nrdDebugView) const;

    void reset();

  private:
    bool m_historyAnchorValid = false;
    glm::vec3 m_historyAnchor = glm::vec3(0.0f);
    glm::vec3 m_prevSunDir = glm::vec3(0.25f, 0.85f, 0.42f);

    bool m_warnedHardwareRtUnavailable = false;
    bool m_loggedTracingBackend = false;
    GiTracingBackend m_lastTracingBackend = GiTracingBackend::SoftwareDda;
};
