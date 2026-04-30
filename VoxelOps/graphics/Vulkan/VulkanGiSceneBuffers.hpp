#pragma once

#include "VulkanChunkRenderCache.hpp"
#include "renderer/RenderFrameData.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <cstddef>
#include <cstdint>

class ChunkManager;
class VulkanContext;

class VulkanGiSceneBuffers {
  public:
    bool rebuild(const ChunkManager &chunkManager, const VulkanChunkRenderCache &chunkRenderCache,
                 VulkanContext &context);
    void applyToLighting(GiLightingData &lighting) const;
    void cleanup();

    [[nodiscard]] bool valid() const noexcept { return m_valid; }
    [[nodiscard]] size_t chunkCount() const noexcept { return m_chunkCount; }

  private:
    vk::raii::Buffer m_occupancyBuffer{nullptr};
    vk::raii::DeviceMemory m_occupancyBufferMemory{nullptr};
    vk::raii::Buffer m_materialBuffer{nullptr};
    vk::raii::DeviceMemory m_materialBufferMemory{nullptr};
    glm::ivec3 m_minBlocks{0};
    glm::uvec3 m_dims{0u};
    glm::ivec4 m_worldBoundsXy{0};
    glm::ivec4 m_worldBoundsZ{0};
    uint32_t m_wordCount = 0;
    uint64_t m_signatureXor = 0;
    uint64_t m_signatureSum = 0;
    size_t m_chunkCount = 0;
    bool m_valid = false;
};
