#pragma once

#include "VulkanChunkRenderCache.hpp"
#include "renderer/RenderFrameData.hpp"
#include "../../render/RenderScene.hpp"

#include <glm/glm.hpp>
#include <vector>

class Camera;
class VkModel;
class VkTexture;
struct ImDrawData;

struct VulkanFrameBuildResult {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 viewProjection{1.0f};
    float cpuFrameBuildMs = 0.0f;
};

class VulkanFrameBuilder {
public:
    static VulkanFrameBuildResult buildFrameData(
        FrameRenderData &frameData,
        const VulkanChunkRenderCache &chunkRenderCache,
        const VkTexture &atlasTexture,
        const Camera &activeCamera,
        const Camera &cullingCamera,
        const glm::vec3 &localPlayerPosition,
        uint16_t chunkRenderDistance,
        const std::vector<RenderRemotePlayerState> &remotePlayers,
        ImDrawData *uiDrawData,
        int width,
        int height,
        const VkModel *remotePlayerModel,
        const std::vector<const VkTexture *> &remotePlayerTextures,
        const glm::ivec3 &cullingChunk
    );
};
