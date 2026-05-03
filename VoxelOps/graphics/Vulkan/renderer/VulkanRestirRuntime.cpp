#include "VulkanRenderer.hpp"
#include "RestirConfig.hpp"

using namespace Vulkan::Restir;

void VulkanRenderer::updateRestirHistoryAfterSubmit(
    const FrameRenderData &frameData,
    const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    const glm::mat4 &viewProjection,
    vk::Fence currentFrameFence
) {
    m_restirSharedHistoryFence =
        frameData.giLighting.pathTracingEnabled ? currentFrameFence : VK_NULL_HANDLE;

    const uint32_t historyIndex = kRestirHistorySlot;
    if (historyIndex < m_restirDiValidPerImage.size()) {
        if (frameData.giLighting.pathTracingEnabled) {
            m_restirDiValidPerImage[historyIndex] = true;
            if (historyIndex < m_restirDiWriteParityPerImage.size()) {
                m_restirDiWriteParityPerImage[historyIndex] ^= 1u;
            }
        } else {
            m_restirDiValidPerImage[historyIndex] = false;
        }
    }
    if (historyIndex < m_restirDiPrevViewProjectionPerImage.size() &&
        historyIndex < m_prevViewProjectionValidPerImage.size() &&
        historyIndex < m_restirDiPrevViewPerImage.size() &&
        historyIndex < m_restirDiPrevProjectionPerImage.size()) {
        if (frameData.giLighting.enabled) {
            m_restirDiPrevViewProjectionPerImage[historyIndex] = viewProjection;
            m_restirDiPrevViewPerImage[historyIndex] = viewMatrix;
            m_restirDiPrevProjectionPerImage[historyIndex] = projectionMatrix;
            m_prevViewProjectionValidPerImage[historyIndex] = true;
        } else {
            m_prevViewProjectionValidPerImage[historyIndex] = false;
        }
    }
    if (frameData.giLighting.enabled) {
        m_nrdPrevViewProjection = viewProjection;
        m_nrdPrevView = viewMatrix;
        m_nrdPrevProjection = projectionMatrix;
        m_nrdPrevMatricesValid = true;
    } else {
        m_nrdPrevMatricesValid = false;
    }
}
