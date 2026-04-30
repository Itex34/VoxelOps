#include "VulkanGiSettings.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
constexpr bool kEnablePathTracedGi = true;
constexpr uint32_t kPathTraceRaysPerPixel = 1u;
constexpr uint32_t kPathTraceMaxBounces = 2u;
constexpr float kPathTraceSkyIntensity = 1.0f;
}

VulkanGiSettings::FrameDecision VulkanGiSettings::beginFrame(
    const glm::vec3 &cameraPosition, const glm::vec3 &sunDirection, bool hardwareRtSupported,
    VkAccelerationStructureKHR sceneTlas, int tracingBackendPreference) {
    FrameDecision out{};
    out.hardwareRtSupported = hardwareRtSupported;
    out.rtSceneReady = hardwareRtSupported && (sceneTlas != VK_NULL_HANDLE);

    const float sunLen2 = glm::dot(sunDirection, sunDirection);
    if (sunLen2 > 1.0e-8f && std::isfinite(sunLen2)) {
        out.sunDirection = glm::normalize(sunDirection);
    } else {
        out.sunDirection = glm::normalize(glm::vec3(0.25f, 0.85f, 0.42f));
    }

    if (m_historyAnchorValid) {
        const float cameraDelta = glm::length(cameraPosition - m_historyAnchor);
        const float sunDelta = 1.0f - glm::clamp(glm::dot(out.sunDirection, m_prevSunDir), -1.0f, 1.0f);
        out.resetHistory = (cameraDelta > 6.0f) || (sunDelta > 0.08f);
    }
    m_historyAnchor = cameraPosition;
    m_prevSunDir = out.sunDirection;
    m_historyAnchorValid = true;

    const int clampedPreference = std::clamp(tracingBackendPreference, 0, 2);
    if (clampedPreference == 1) {
        out.backend = GiTracingBackend::SoftwareDda;
    } else if (clampedPreference == 2) {
        out.backend =
            out.rtSceneReady ? GiTracingBackend::HardwareRt : GiTracingBackend::SoftwareDda;
    } else {
        out.backend =
            out.rtSceneReady ? GiTracingBackend::HardwareRt : GiTracingBackend::SoftwareDda;
    }

    if (!out.rtSceneReady && !m_warnedHardwareRtUnavailable) {
        std::cerr << "[Vulkan][GI] Hardware RT scene unavailable (support="
                  << (hardwareRtSupported ? "yes" : "no")
                  << ", scene=" << ((sceneTlas != VK_NULL_HANDLE) ? "ready" : "not-ready")
                  << "). GI path tracing disabled for this frame.\n";
        m_warnedHardwareRtUnavailable = true;
    }
    const bool backendChanged = m_loggedTracingBackend && (out.backend != m_lastTracingBackend);
    if (backendChanged) {
        out.resetHistory = true;
    }

    if (!m_loggedTracingBackend || out.backend != m_lastTracingBackend) {
        std::cout << "[Vulkan][GI] tracing backend="
                  << ((out.backend == GiTracingBackend::HardwareRt) ? "HardwareRt" : "SoftwareDda")
                  << " hwRtSupported=" << (hardwareRtSupported ? "true" : "false")
                  << " sceneTlas=" << ((sceneTlas != VK_NULL_HANDLE) ? "ready" : "none")
                  << " preference=" << clampedPreference
                  << "\n";
        m_loggedTracingBackend = true;
        m_lastTracingBackend = out.backend;
    }

    if (!out.rtSceneReady) {
        out.resetHistory = true;
    }
    return out;
}

void VulkanGiSettings::fillLightingData(GiLightingData &lighting, const FrameDecision &decision,
                                        uint32_t nrdDebugView) const {
    lighting.hardwareRayTracingSupported = decision.hardwareRtSupported;
    lighting.tracingBackend = decision.backend;
    lighting.pathTracingEnabled = kEnablePathTracedGi;
    lighting.pathTraceRaysPerPixel = kPathTraceRaysPerPixel;
    lighting.pathTraceMaxBounces = kPathTraceMaxBounces;
    lighting.pathTraceSkyIntensity = kPathTraceSkyIntensity;
    lighting.baseDiffuse = kEnablePathTracedGi ? 0.12f : 0.50f;
    lighting.giIntensity = 1.00f;
    lighting.sunIntensity = 1.35f;
    lighting.restirTemporalBlend = 0.86f;
    lighting.restirSpatialReuse = 0.18f;
    lighting.denoiseTemporalBlend = 0.92f;
    lighting.denoiseSpatialWeight = 0.26f;
    lighting.denoiseLumaPhi = 2.0f;
    lighting.denoiseMomentBlend = 0.08f;
    lighting.sunShadowMinVisibility = 0.00f;
    lighting.sunShadowMaxDistance = 256.0f;
    lighting.sunDirection = decision.sunDirection;
    lighting.sunShadowsEnabled = false;
    lighting.resetHistory = decision.resetHistory;
    lighting.nrdDebugView = nrdDebugView;
    lighting.traceMaterialBuffer = VK_NULL_HANDLE;
    lighting.shadowOccupancyBuffer = VK_NULL_HANDLE;
    lighting.shadowOccupancyMinBlocks = glm::ivec3(0);
    lighting.shadowOccupancyDims = glm::uvec3(0u);
    lighting.shadowOccupancyWordCount = 0;
    lighting.shadowWorldBoundsXy = glm::ivec4(0);
    lighting.shadowWorldBoundsZ = glm::ivec4(0);
    lighting.enabled = kEnablePathTracedGi;
}

void VulkanGiSettings::reset() {
    m_historyAnchorValid = false;
    m_historyAnchor = glm::vec3(0.0f);
    m_prevSunDir = glm::vec3(0.25f, 0.85f, 0.42f);
    m_warnedHardwareRtUnavailable = false;
    m_loggedTracingBackend = false;
    m_lastTracingBackend = GiTracingBackend::SoftwareDda;
}
