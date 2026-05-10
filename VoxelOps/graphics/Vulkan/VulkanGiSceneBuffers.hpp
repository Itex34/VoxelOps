#pragma once

#include "VulkanChunkRenderCache.hpp"
#include "renderer/RenderFrameData.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../../voxels/Chunk.hpp"
#include "../../voxels/VoxelCoordHash.hpp"

class VulkanContext;

class VulkanGiSceneBuffers {
public:
    bool rebuild(
        const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &chunks,
        const VulkanChunkRenderCache &chunkRenderCache,
        VulkanContext &context,
        uint64_t frameCounter
    );
    void applyToLighting(GiLightingData &lighting) const;
    void collectRetiredBuffers(uint64_t frameCounter);
    void cleanup();

    [[nodiscard]] bool valid() const noexcept {
        return m_valid;
    }
    [[nodiscard]] size_t chunkCount() const noexcept {
        return m_chunkCount;
    }
    [[nodiscard]] uint64_t contentVersion() const noexcept {
        return m_contentVersion;
    }

private:
    struct RetiredBuffers {
        vk::raii::Buffer occupancyBuffer{nullptr};
        vk::raii::DeviceMemory occupancyBufferMemory{nullptr};
        vk::raii::Buffer materialBuffer{nullptr};
        vk::raii::DeviceMemory materialBufferMemory{nullptr};
        uint64_t retireFrame = 0;
    };

    void retireActiveBuffers(uint64_t frameCounter);
    void resetActiveState();

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
    uint64_t m_contentVersion = 0;
    uint64_t m_lastRebuildFrame = 0;
    size_t m_chunkCount = 0;
    bool m_valid = false;
    std::vector<RetiredBuffers> m_retiredBuffers;
};
