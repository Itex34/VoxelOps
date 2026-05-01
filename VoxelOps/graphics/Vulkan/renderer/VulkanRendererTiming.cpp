#include "VulkanRenderer.hpp"
#include "../vulkan/VulkanContext.hpp"


void VulkanRenderer::updateGpuTimingStatsForImage(uint32_t imageIndex) {
    if (!m_timestampQueriesEnabled || imageIndex >= m_timestampQueryPools.size()) {
        m_lastFrameTimingStats.gpuValid = false;
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    const vk::QueryPool queryPool = *m_timestampQueryPools[imageIndex];
    std::array<uint64_t, TIMESTAMP_QUERY_COUNT> ticks{};
    const VkResult result =
        vkGetQueryPoolResults(static_cast<VkDevice>(*device), static_cast<VkQueryPool>(queryPool),
                              0, TIMESTAMP_QUERY_COUNT, sizeof(ticks), ticks.data(),
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (result != VK_SUCCESS) {
        m_lastFrameTimingStats.gpuValid = false;
        return;
    }

    const double tickToMs = static_cast<double>(m_timestampPeriodNanoseconds) * 1.0e-6;
    const auto deltaMs = [tickToMs](uint64_t startTick, uint64_t endTick) -> float {
        if (endTick < startTick) {
            return 0.0f;
        }
        return static_cast<float>(static_cast<double>(endTick - startTick) * tickToMs);
    };

    m_lastFrameTimingStats.gpuValid = true;
    m_lastFrameTimingStats.gpuChunkPassMs = deltaMs(ticks[0], ticks[1]);
    m_lastFrameTimingStats.gpuModelPassMs = deltaMs(ticks[1], ticks[2]);
    m_lastFrameTimingStats.gpuUiPassMs = deltaMs(ticks[2], ticks[3]);
    m_lastFrameTimingStats.gpuFrameMs = deltaMs(ticks[0], ticks[4]);
}
