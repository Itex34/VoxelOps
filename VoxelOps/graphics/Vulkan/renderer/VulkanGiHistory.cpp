#include "VulkanRenderer.hpp"

void VulkanRenderer::updateGiHistoryAfterSubmit(
    const FrameRenderData &frameData,
    const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    const glm::mat4 &viewProjection,
    vk::Fence currentFrameFence
) {
    (void)currentFrameFence;
    if (frameData.giLighting.enabled) {
        m_nrdPrevViewProjection = viewProjection;
        m_nrdPrevView = viewMatrix;
        m_nrdPrevProjection = projectionMatrix;
        m_nrdPrevMatricesValid = true;
    } else {
        m_nrdPrevMatricesValid = false;
    }
}
