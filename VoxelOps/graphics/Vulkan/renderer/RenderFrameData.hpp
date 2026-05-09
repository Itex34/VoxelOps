#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class VkMesh;
class VkModel;
class VkTexture;
struct ImDrawData;

struct RenderObject {
    const VkModel *model = nullptr;
    const std::vector<const VkTexture *> *meshTextures = nullptr;
    glm::mat4 transform{1.0f};
};

// matches VkDrawIndexedIndirectCommand and DrawElementsIndirectCommand layouts.
struct IndexedIndirectCommand {
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;
};

struct RenderIndirectBatch {
    const VkMesh *mesh = nullptr;
    const VkTexture *texture = nullptr;
    uint32_t firstCommand = 0;
    uint32_t commandCount = 0;
};

enum class GiTracingBackend : uint32_t { SoftwareDda = 0u, HardwareRt = 1u };

struct GiLightingData {
    bool enabled = false;
    bool pathTracingEnabled = false;
    bool resetHistory = false;
    bool hardwareRayTracingSupported = false;
    GiTracingBackend tracingBackend = GiTracingBackend::SoftwareDda;
    uint32_t pathTraceRaysPerPixel = 1;
    uint32_t pathTraceMaxBounces = 2;
    float baseDiffuse = 1.0f;
    float giIntensity = 0.55f;
    float sunIntensity = 0.70f;
    float pathTraceSkyIntensity = 1.0f;
    float sunShadowMinVisibility = 0.00f;
    float sunShadowMaxDistance = 64.0f;
    float denoiseTemporalBlend = 0.86f;
    float denoiseSpatialWeight = 0.22f;
    float denoiseLumaPhi = 1.6f;
    float denoiseMomentBlend = 0.12f;
    // Matches NRD ReblurHitDistanceParameters { A, B, C }.
    glm::vec3 nrdHitDistanceParams{3.0f, 0.1f, 20.0f};
    // 0=off, 1=diff radiance, 2=hit distance, 3=normal, 4=motion, 5=viewZ, 6=raw noisy signal
    uint32_t nrdDebugView = 0;
    // 0=off, 1=flat normal+roughness, 2=flat normal+roughness + zero motion
    uint32_t nrdGuideOverride = 0;
    glm::vec3 sunDirection{0.25f, 0.85f, 0.42f};
    bool sunShadowsEnabled = false;
    glm::ivec3 shadowOccupancyMinBlocks{0};
    glm::uvec3 shadowOccupancyDims{0u};
    uint32_t shadowOccupancyWordCount = 0;
    glm::ivec4 shadowWorldBoundsXy{0}; // x=minX,y=maxX,z=minY,w=maxY
    glm::ivec4 shadowWorldBoundsZ{0};  // x=minZ,y=maxZ
    VkBuffer shadowOccupancyBuffer = VK_NULL_HANDLE;
    VkBuffer traceMaterialBuffer = VK_NULL_HANDLE;
    VkAccelerationStructureKHR sceneTlas = VK_NULL_HANDLE;
};

struct FrameRenderData {
    std::vector<RenderObject> objects;

    // VkModel matrices consumed by direct and indirect draws (indexed by firstInstance).
    std::vector<glm::mat4> modelMatrices;

    // Region-level indirect commands.
    std::vector<IndexedIndirectCommand> indirectCommands;
    std::vector<RenderIndirectBatch> indirectBatches;
    ImDrawData *uiDrawData = nullptr;
    GiLightingData giLighting{};

    void clear() {
        objects.clear();
        modelMatrices.clear();
        indirectCommands.clear();
        indirectBatches.clear();
        uiDrawData = nullptr;
        giLighting = GiLightingData{};
    }
};
