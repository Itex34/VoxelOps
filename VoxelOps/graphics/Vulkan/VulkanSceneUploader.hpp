#pragma once

#include "VulkanChunkRenderCache.hpp"
#include "VulkanRayTracingScene.hpp"
#include "graphics/Model.hpp"
#include "graphics/VkTexture.hpp"
#include "vulkan/UploadContext.hpp"

#include <glm/fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class ChunkManager;
class VulkanContext;

class VulkanSceneUploader {
  public:
    bool initialize(VulkanContext &context, UploadContext &uploadContext);
    void shutdown();

    void collectRetiredChunkMeshes(uint64_t frameCounter);
    void syncChunkCache(ChunkManager &chunkManager, const glm::ivec3 &cullingChunk,
                        uint64_t frameCounter, VulkanContext &context,
                        UploadContext &uploadContext, VulkanRayTracingScene &rtScene);
    void cleanupChunkMeshes();

    bool ensureRemotePlayerAssetsLoaded(VulkanContext &context, UploadContext &uploadContext);

    [[nodiscard]] bool empty() const noexcept { return m_chunkRenderCache.empty(); }
    [[nodiscard]] size_t size() const noexcept { return m_chunkRenderCache.size(); }
    [[nodiscard]] const VulkanChunkRenderCache &chunkRenderCache() const noexcept {
        return m_chunkRenderCache;
    }
    [[nodiscard]] const auto &chunkMeshes() const noexcept { return m_chunkRenderCache.getChunkMeshes(); }

    [[nodiscard]] const VkTexture &atlasTexture() const noexcept { return m_atlasTexture; }
    [[nodiscard]] const VkModel *remotePlayerModel() const noexcept { return m_remotePlayerModel.get(); }
    [[nodiscard]] const std::vector<const VkTexture *> &remotePlayerTextureViews() const noexcept {
        return m_remotePlayerTextureViews;
    }

  private:
    bool ensureAtlasTextureLoaded(VulkanContext &context, UploadContext &uploadContext);
    void cleanupRemotePlayerAssets();

    VulkanChunkRenderCache m_chunkRenderCache;
    VkTexture m_atlasTexture;
    bool m_atlasTextureLoaded = false;
    std::unique_ptr<VkModel> m_remotePlayerModel;
    std::vector<VkTexture> m_remotePlayerTextures;
    std::vector<const VkTexture *> m_remotePlayerTextureViews;
    bool m_remotePlayerAssetsLoaded = false;
    bool m_warnedRemotePlayerAssets = false;
};
