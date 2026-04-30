#pragma once

#include <glm/fwd.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

class VulkanContext;
class UploadContext;
struct CpuChunkMesh;

class VulkanRayTracingScene {
  public:
    VulkanRayTracingScene();
    ~VulkanRayTracingScene();

    VulkanRayTracingScene(const VulkanRayTracingScene &) = delete;
    VulkanRayTracingScene &operator=(const VulkanRayTracingScene &) = delete;
    VulkanRayTracingScene(VulkanRayTracingScene &&) = delete;
    VulkanRayTracingScene &operator=(VulkanRayTracingScene &&) = delete;

    void initialize(VulkanContext &context, UploadContext &uploadContext, uint64_t frameCounter);
    void collectRetiredResources(uint64_t frameCounter);
    void reset();

    bool uploadChunkGeometry(VulkanContext &context, UploadContext &uploadContext,
                             uint64_t frameCounter, const glm::ivec3 &chunkPos,
                             const CpuChunkMesh &cpuMesh);
    void removeChunkGeometry(uint64_t frameCounter, const glm::ivec3 &chunkPos);
    bool rebuild(VulkanContext &context, uint64_t frameCounter);

    [[nodiscard]] bool isDirty() const noexcept { return m_dirty; }
    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] VkAccelerationStructureKHR activeTlas() const noexcept {
        return m_activeTlas;
    }

  private:
    struct RtSceneState;

    std::unique_ptr<RtSceneState> m_state;
    bool m_dirty = false;
    VkAccelerationStructureKHR m_activeTlas = VK_NULL_HANDLE;
};
