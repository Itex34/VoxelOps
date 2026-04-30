#include "VulkanSceneUploader.hpp"

#include "vulkan/VulkanContext.hpp"
#include "../AtlasLayout.hpp"
#include "../../world/ChunkManager.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <exception>
#include <iostream>
#include <string>

bool VulkanSceneUploader::initialize(VulkanContext &context, UploadContext &uploadContext) {
    if (!ensureAtlasTextureLoaded(context, uploadContext)) {
        return false;
    }
    (void)ensureRemotePlayerAssetsLoaded(context, uploadContext);
    return true;
}

void VulkanSceneUploader::shutdown() {
    cleanupChunkMeshes();
    cleanupRemotePlayerAssets();
    m_atlasTexture.cleanup();
    m_atlasTextureLoaded = false;
}

void VulkanSceneUploader::collectRetiredChunkMeshes(uint64_t frameCounter) {
    m_chunkRenderCache.collectRetiredChunkMeshes(frameCounter);
}

void VulkanSceneUploader::syncChunkCache(ChunkManager &chunkManager, const glm::ivec3 &cullingChunk,
                                         uint64_t frameCounter, VulkanContext &context,
                                         UploadContext &uploadContext,
                                         VulkanRayTracingScene &rtScene) {
    m_chunkRenderCache.syncFromChunkManager(
        chunkManager, cullingChunk, frameCounter, context, uploadContext,
        [&rtScene, frameCounter](const glm::ivec3 &chunkPos) {
            rtScene.removeChunkGeometry(frameCounter, chunkPos);
        },
        [&rtScene, &context, &uploadContext, frameCounter](const glm::ivec3 &chunkPos,
                                                            const CpuChunkMesh &cpuMesh) {
            if (context.isHardwareRayTracingSupported()) {
                (void)rtScene.uploadChunkGeometry(context, uploadContext, frameCounter, chunkPos,
                                                  cpuMesh);
            }
        });
}

void VulkanSceneUploader::cleanupChunkMeshes() {
    m_chunkRenderCache.cleanup();
}

bool VulkanSceneUploader::ensureRemotePlayerAssetsLoaded(VulkanContext &context,
                                                         UploadContext &uploadContext) {
    if (m_remotePlayerAssetsLoaded) {
        return true;
    }

    try {
        cleanupRemotePlayerAssets();

        const std::string modelPath =
            Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();
        m_remotePlayerModel = std::make_unique<VkModel>();
        m_remotePlayerModel->loadModel(modelPath);
        m_remotePlayerModel->initGpuResources(context.getDevice(), context.getPhysicalDevice(),
                                              uploadContext);

        const auto &meshTexturePaths = m_remotePlayerModel->getMeshTexturePaths();
        m_remotePlayerTextures.clear();
        m_remotePlayerTextures.reserve(meshTexturePaths.size());
        for (const std::string &texturePath : meshTexturePaths) {
            VkTexture texture{};
            texture.initFromFile(context.getDevice(), context.getPhysicalDevice(), uploadContext,
                                 texturePath, context.isSamplerAnisotropyEnabled(),
                                 context.getMaxSamplerAnisotropy());
            m_remotePlayerTextures.emplace_back(std::move(texture));
        }

        m_remotePlayerTextureViews.clear();
        m_remotePlayerTextureViews.reserve(m_remotePlayerTextures.size());
        for (const VkTexture &texture : m_remotePlayerTextures) {
            m_remotePlayerTextureViews.push_back(&texture);
        }

        uploadContext.waitIdle();
        m_remotePlayerAssetsLoaded = true;
        m_warnedRemotePlayerAssets = false;
        return true;
    } catch (const std::exception &e) {
        if (!m_warnedRemotePlayerAssets) {
            std::cerr << "[Vulkan] Failed to load remote player model assets: " << e.what() << "\n";
            m_warnedRemotePlayerAssets = true;
        }
        cleanupRemotePlayerAssets();
        return false;
    }
}

bool VulkanSceneUploader::ensureAtlasTextureLoaded(VulkanContext &context, UploadContext &uploadContext) {
    if (m_atlasTextureLoaded) {
        return true;
    }

    const std::string atlasPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("assets/textures/textureAtlas.png")
            .generic_string();
    m_atlasTexture.initFromAtlasFileAsArray(
        context.getDevice(), context.getPhysicalDevice(), uploadContext, atlasPath,
        static_cast<uint32_t>(TEXTURE_ATLAS_SIZE), context.isSamplerAnisotropyEnabled(),
        context.getMaxSamplerAnisotropy());
    m_atlasTextureLoaded = true;
    return true;
}

void VulkanSceneUploader::cleanupRemotePlayerAssets() {
    for (VkTexture &texture : m_remotePlayerTextures) {
        texture.cleanup();
    }
    m_remotePlayerTextures.clear();
    m_remotePlayerTextureViews.clear();
    if (m_remotePlayerModel) {
        m_remotePlayerModel->cleanupGpuResources();
        m_remotePlayerModel.reset();
    }
    m_remotePlayerAssetsLoaded = false;
}
