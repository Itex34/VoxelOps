#pragma once

#include "../../world/ChunkManager.hpp"
#include "graphics/Mesh.hpp"
#include "vulkan/UploadContext.hpp"

#include <functional>
#include <unordered_map>
#include <vector>

class VulkanContext;

class VulkanChunkRenderCache {
  public:
    struct CachedChunkMesh {
        VkMesh mesh;
        uint64_t revision = 0;
    };

    void collectRetiredChunkMeshes(uint64_t frameCounter);
    void syncFromChunkManager(
        ChunkManager &chunkManager, const glm::ivec3 &cullingChunk, uint64_t frameCounter,
        VulkanContext &context, UploadContext &uploadContext,
        const std::function<void(const glm::ivec3 &)> &onChunkRemoved,
        const std::function<void(const glm::ivec3 &, const CpuChunkMesh &)> &onChunkUploaded);
    void cleanup();

    [[nodiscard]] bool empty() const noexcept { return m_chunkMeshes.empty(); }
    [[nodiscard]] size_t size() const noexcept { return m_chunkMeshes.size(); }
    [[nodiscard]] const std::unordered_map<glm::ivec3, CachedChunkMesh, IVec3Hash> &
    getChunkMeshes() const noexcept {
        return m_chunkMeshes;
    }

  private:
    struct RetiredChunkMesh {
        VkMesh mesh;
        uint64_t retireFrame = 0;
    };

    void retireChunkMesh(VkMesh &&mesh, uint64_t frameCounter);

    std::unordered_map<glm::ivec3, CachedChunkMesh, IVec3Hash> m_chunkMeshes;
    std::vector<RetiredChunkMesh> m_retiredChunkMeshes;
};
