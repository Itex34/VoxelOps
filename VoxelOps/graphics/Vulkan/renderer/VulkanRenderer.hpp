#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "graphics/Vulkan/renderer/IRenderBackend.hpp"
#include "graphics/Vulkan/renderer/FrameSync.hpp"
#include "graphics/Vulkan/renderer/Pipeline.hpp"
#include "graphics/Vulkan/renderer/RenderPass.hpp"
#include "graphics/Vulkan/graphics/Mesh.hpp"
#include "graphics/Vulkan/graphics/VkTexture.hpp"
#include "graphics/Vulkan/vulkan/UploadContext.hpp"

#ifndef VOXELOPS_NRD_HEADERS
#define VOXELOPS_NRD_HEADERS 0
#endif

class VulkanContext;
class NrdBootstrap;

class VulkanRenderer final : public IRenderBackend {
  public:
    struct FrameTimingStats {
        bool gpuValid = false;
        float gpuFrameMs = 0.0f;
        float gpuChunkPassMs = 0.0f;
        float gpuModelPassMs = 0.0f;
        float gpuUiPassMs = 0.0f;

        float cpuCommandRecordMs = 0.0f;
        float cpuChunkPassMs = 0.0f;
        float cpuModelPassMs = 0.0f;
        float cpuUiPassMs = 0.0f;
    };

    struct PingPongImageResources {
        vk::raii::Image image{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::ImageView view{nullptr};
    };

    explicit VulkanRenderer(VulkanContext &context);
    ~VulkanRenderer() noexcept;

    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;
    VulkanRenderer(VulkanRenderer &&) = delete;
    VulkanRenderer &operator=(VulkanRenderer &&) = delete;

    void init() override;
    void renderFrame(uint32_t windowWidth, uint32_t windowHeight, const glm::mat4 &viewMatrix,
                     const glm::mat4 &projectionMatrix, const glm::mat4 &viewProjection,
                     const FrameRenderData &frameData) override;
    void handleWindowResize(uint32_t windowWidth, uint32_t windowHeight) override;
    void cleanup() override;
    vk::RenderPass getRenderPassHandle() const noexcept;
    uint32_t getSwapchainImageCount() const noexcept;
    const FrameTimingStats &getLastFrameTimingStats() const noexcept {
        return m_lastFrameTimingStats;
    }
    bool isNrdBootstrapActive() const noexcept;
    uint32_t getNrdBootstrapDispatchCount() const noexcept;

  private:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
    static constexpr vk::DeviceSize MIN_MODEL_BUFFER_BYTES = sizeof(glm::mat4);
    static constexpr vk::DeviceSize MIN_INDIRECT_BUFFER_BYTES = sizeof(IndexedIndirectCommand);
    static constexpr uint32_t TIMESTAMP_QUERY_COUNT = 5;
    static constexpr uint32_t GI_CASCADE_BINDINGS = GI_LIGHTING_MAX_CASCADES;
    static constexpr uint32_t GI_PARAM_BINDING = GI_CASCADE_BINDINGS;
    static constexpr uint32_t GI_SHADOW_OCCUPANCY_BINDING = GI_CASCADE_BINDINGS + 1;
    static constexpr uint32_t GI_MATERIAL_BINDING = GI_CASCADE_BINDINGS + 2;
    static constexpr uint32_t GI_RESTIR_DI_PREV_BINDING = GI_CASCADE_BINDINGS + 3;
    static constexpr uint32_t GI_RESTIR_DI_CURR_BINDING = GI_CASCADE_BINDINGS + 4;
    static constexpr uint32_t GI_RESTIR_VALIDATION_PREV_BINDING = GI_CASCADE_BINDINGS + 5;
    static constexpr uint32_t GI_RESTIR_VALIDATION_CURR_BINDING = GI_CASCADE_BINDINGS + 6;
    static constexpr uint32_t GI_RESTIR_META_PREV_BINDING = GI_CASCADE_BINDINGS + 7;
    static constexpr uint32_t GI_RESTIR_META_CURR_BINDING = GI_CASCADE_BINDINGS + 8;
    static constexpr uint32_t GI_RESTIR_GI_PREV_BINDING = GI_CASCADE_BINDINGS + 9;
    static constexpr uint32_t GI_RESTIR_GI_CURR_BINDING = GI_CASCADE_BINDINGS + 10;
    static constexpr uint32_t GI_RESTIR_GI_META_PREV_BINDING = GI_CASCADE_BINDINGS + 11;
    static constexpr uint32_t GI_RESTIR_GI_META_CURR_BINDING = GI_CASCADE_BINDINGS + 12;
    static constexpr uint32_t GI_NRD_DIFF_IN_BINDING = GI_CASCADE_BINDINGS + 13;
    static constexpr uint32_t GI_NRD_NORMAL_ROUGHNESS_IN_BINDING = GI_CASCADE_BINDINGS + 14;
    static constexpr uint32_t GI_NRD_MV_IN_BINDING = GI_CASCADE_BINDINGS + 15;
    static constexpr uint32_t GI_NRD_VIEWZ_IN_BINDING = GI_CASCADE_BINDINGS + 16;
    static constexpr uint32_t GI_NRD_DIFF_OUT_BINDING = GI_CASCADE_BINDINGS + 17;
    static constexpr uint32_t GI_RT_SCENE_BINDING = GI_CASCADE_BINDINGS + 18;
    static constexpr uint32_t GI_BINDING_COUNT = GI_CASCADE_BINDINGS + 19;

    VulkanContext &m_context;
    std::unique_ptr<NrdBootstrap> m_nrdBootstrap;
    bool m_initialized = false;

    vk::raii::CommandPool m_commandPool{nullptr};
    std::vector<vk::raii::CommandBuffer> m_commandBuffers;
    std::vector<vk::raii::Framebuffer> m_framebuffers;

    RenderPass m_renderPass;
    Pipeline m_chunkPipeline;
    Pipeline m_modelPipeline;
    FrameSync m_frameSync;
    UploadContext m_uploadContext;
    VkTexture m_fallbackArrayTexture;
    VkTexture m_fallback2DTexture;

    struct PerImageDrawResources {
        vk::raii::Buffer modelMatrixBuffer{nullptr};
        vk::raii::DeviceMemory modelMatrixBufferMemory{nullptr};
        vk::DeviceSize modelMatrixCapacityBytes = 0;
        void *modelMatrixMapped = nullptr;

        vk::raii::Buffer indirectCommandBuffer{nullptr};
        vk::raii::DeviceMemory indirectCommandBufferMemory{nullptr};
        vk::DeviceSize indirectCommandCapacityBytes = 0;
        void *indirectCommandMapped = nullptr;

        vk::raii::Buffer giParamsBuffer{nullptr};
        vk::raii::DeviceMemory giParamsBufferMemory{nullptr};
        void *giParamsMapped = nullptr;
    };
    std::vector<PerImageDrawResources> m_perImageDrawResources;

    vk::raii::DescriptorSetLayout m_modelDescriptorSetLayout{nullptr};
    vk::raii::DescriptorPool m_modelDescriptorPool{nullptr};
    std::vector<vk::raii::DescriptorSet> m_modelDescriptorSets;
    vk::raii::DescriptorSetLayout m_giDescriptorSetLayout{nullptr};
    vk::raii::DescriptorPool m_giDescriptorPool{nullptr};
    std::vector<vk::raii::DescriptorSet> m_giDescriptorSets;
    bool m_giRtDescriptorEnabled = false;
    std::vector<vk::raii::Buffer> m_giFallbackProbeBuffers;
    std::vector<vk::raii::DeviceMemory> m_giFallbackProbeBufferMemory;
    vk::raii::Buffer m_giFallbackShadowOccupancyBuffer{nullptr};
    vk::raii::DeviceMemory m_giFallbackShadowOccupancyBufferMemory{nullptr};
    vk::raii::Buffer m_giFallbackMaterialBuffer{nullptr};
    vk::raii::DeviceMemory m_giFallbackMaterialBufferMemory{nullptr};
    struct RestirDiReservoirResources {
        vk::raii::Image image{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    std::vector<std::array<RestirDiReservoirResources, 2>> m_restirDiPerImage;
    std::vector<uint32_t> m_restirDiWriteParityPerImage;
    std::vector<bool> m_restirDiValidPerImage;
    std::vector<glm::mat4> m_restirDiPrevViewProjectionPerImage;
    std::vector<glm::mat4> m_restirDiPrevViewPerImage;
    std::vector<glm::mat4> m_restirDiPrevProjectionPerImage;
    std::vector<bool> m_prevViewProjectionValidPerImage;
    glm::mat4 m_nrdPrevViewProjection{1.0f};
    glm::mat4 m_nrdPrevView{1.0f};
    glm::mat4 m_nrdPrevProjection{1.0f};
    bool m_nrdPrevMatricesValid = false;
    vk::raii::Sampler m_restirDiSampler{nullptr};
    struct RestirValidationResources {
        vk::raii::Image image{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    std::vector<std::array<RestirValidationResources, 2>> m_restirValidationPerImage;
    vk::raii::Sampler m_restirValidationSampler{nullptr};
    struct RestirMetaResources {
        vk::raii::Image image{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    std::vector<std::array<RestirMetaResources, 2>> m_restirMetaPerImage;
    vk::raii::Sampler m_restirMetaSampler{nullptr};
    std::vector<std::array<RestirDiReservoirResources, 2>> m_restirGiPerImage;
    vk::raii::Sampler m_restirGiSampler{nullptr};
    std::vector<std::array<RestirMetaResources, 2>> m_restirGiMetaPerImage;
    vk::raii::Sampler m_restirGiMetaSampler{nullptr};
    std::vector<std::array<RestirDiReservoirResources, 2>> m_restirGiSpatialPerImage;
    std::vector<std::array<RestirMetaResources, 2>> m_restirGiSpatialMetaPerImage;
    struct NrdPerImageResources {
        RestirDiReservoirResources diffIn;
        RestirDiReservoirResources normalRoughnessIn;
        RestirDiReservoirResources motionIn;
        RestirDiReservoirResources viewZIn;
        RestirDiReservoirResources diffOut;
    };
    std::vector<NrdPerImageResources> m_nrdPerImage;
    NrdPerImageResources m_nrdFallback{};
    bool m_nrdFallbackReady = false;
    bool m_loggedMissingNrdResources = false;
    std::vector<bool> m_nrdValidPerImage;
    vk::raii::Sampler m_nrdOutputSampler{nullptr};
    vk::raii::Sampler m_nrdNearestSampler{nullptr};
    vk::raii::Sampler m_nrdLinearSampler{nullptr};
#if VOXELOPS_NRD_HEADERS
    struct NrdRuntimeTexture {
        vk::Format format = vk::Format::eUndefined;
        uint32_t width = 0;
        uint32_t height = 0;
        RestirDiReservoirResources image;
    };
    struct NrdRuntimePerImage {
        std::vector<NrdRuntimeTexture> permanentPool;
        std::vector<NrdRuntimeTexture> transientPool;
        vk::raii::Buffer constantBuffer{nullptr};
        vk::raii::DeviceMemory constantMemory{nullptr};
        void *constantMapped = nullptr;
        vk::raii::DescriptorPool descriptorPool{nullptr};
        std::vector<vk::raii::DescriptorSet> resourcesSets;
        vk::raii::DescriptorSet constantsSet{nullptr};
    };
    std::vector<NrdRuntimePerImage> m_nrdRuntimePerImage;
    std::vector<vk::raii::Pipeline> m_nrdPipelines;
    vk::raii::DescriptorSetLayout m_nrdResourcesSetLayout{nullptr};
    vk::raii::DescriptorSetLayout m_nrdConstantsSetLayout{nullptr};
    vk::raii::PipelineLayout m_nrdPipelineLayout{nullptr};
    uint32_t m_nrdTextureBinding = 0;
    uint32_t m_nrdStorageBinding = 0;
    uint32_t m_nrdConstantBinding = 0;
    uint32_t m_nrdSetResourcesIndex = 0;
    uint32_t m_nrdSetConstantsIndex = 0;
    uint32_t m_nrdTextureCapacity = 0;
    uint32_t m_nrdStorageCapacity = 0;
    uint32_t m_nrdConstantBufferStride = 0;
    uint32_t m_nrdConstantBufferSize = 0;
    bool m_nrdRuntimeReady = false;
#endif
    vk::raii::DescriptorSetLayout m_restirGiSpatialDescriptorSetLayout{nullptr};
    vk::raii::DescriptorPool m_restirGiSpatialDescriptorPool{nullptr};
    std::vector<vk::raii::DescriptorSet> m_restirGiSpatialDescriptorSets;
    vk::raii::PipelineLayout m_restirGiSpatialPipelineLayout{nullptr};
    vk::raii::Pipeline m_restirGiSpatialPipeline{nullptr};
    uint32_t m_frameCounterLow = 0;
    std::vector<vk::raii::QueryPool> m_timestampQueryPools;
    bool m_timestampQueriesEnabled = false;
    float m_timestampPeriodNanoseconds = 0.0f;
    FrameTimingStats m_lastFrameTimingStats{};
    vk::Fence m_restirSharedHistoryFence = VK_NULL_HANDLE;

    vk::raii::Sampler createSharedRestirSampler();

    void createCommandPool();
    void createCommandBuffers();
    void createFramebuffers();
    void createModelDescriptorResources();
    void createGiDescriptorResources();
    void createRestirDiResources();
    void cleanupRestirDiResources();
    void createRestirValidationResources();
    void cleanupRestirValidationResources();
    void createRestirMetaResources();
    void cleanupRestirMetaResources();
    void createRestirGiResources();
    void cleanupRestirGiResources();
    void createNrdSignalResources();
    void cleanupNrdSignalResources();

    template <typename T>
    void createPingPongImagePair(std::vector<std::array<T, 2>> &out, vk::Format format,
                                 vk::Extent2D extent, vk::ImageUsageFlags usage,
                                 const vk::ClearColorValue &clearValue,
                                 vk::raii::CommandBuffer &commandBuffer);

#if VOXELOPS_NRD_HEADERS
    bool createNrdRuntimeResources();
    void cleanupNrdRuntimeResources();
    void dispatchNrdPass(uint32_t imageIndex, const FrameRenderData &frameData);
#endif

    void createRestirGiSpatialResources();
    void cleanupRestirGiSpatialResources();
    void updateRestirGiSpatialDescriptorSet(uint32_t imageIndex, uint32_t writeParity);
    void dispatchRestirGiSpatialPass(uint32_t imageIndex, const FrameRenderData &frameData);
    void clearTemporalGiWriteTargets(uint32_t imageIndex);
    void barrierNrdSignalsForCompute(uint32_t imageIndex);
    void createTimestampResources();
    void cleanupTimestampResources();
    void updateGpuTimingStatsForImage(uint32_t imageIndex);
    void ensurePerImageDrawBufferCapacity(uint32_t imageIndex, vk::DeviceSize modelBytes,
                                          vk::DeviceSize indirectBytes);
    void updatePerImageDrawBuffers(uint32_t imageIndex, const std::vector<glm::mat4> &modelMatrices,
                                   const std::vector<IndexedIndirectCommand> &indirectCommands);
    void updateModelDescriptorSet(uint32_t imageIndex);
    void updateGiDescriptorSet(uint32_t imageIndex, const FrameRenderData &frameData,
                               const glm::mat4 &viewProjection);
    void cleanupPerImageDrawResources();
    void cleanupModelDescriptorResources();
    void cleanupGiDescriptorResources();

    void recreateSwapchainDependentResources();
    void cleanupSwapchainDependentResources();

    void recordCommandBuffer(uint32_t imageIndex, const glm::mat4 &viewProjection,
                             const FrameRenderData &frameData, float &outChunkCpuMs,
                             float &outModelCpuMs, float &outUiCpuMs);
};
