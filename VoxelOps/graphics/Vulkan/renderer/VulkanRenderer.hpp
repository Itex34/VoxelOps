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
        float gpuMainSetupMs = 0.0f;
        float gpuChunkDrawMs = 0.0f;
        float gpuModelPassMs = 0.0f;
        float gpuUiPassMs = 0.0f;
        float gpuMainTailMs = 0.0f;
        float gpuMainPassMs = 0.0f;
        float gpuNrdDispatchMs = 0.0f;
        float gpuCompositePassMs = 0.0f;

        float cpuCommandRecordMs = 0.0f;
        float cpuChunkPassMs = 0.0f;
        float cpuModelPassMs = 0.0f;
        float cpuUiPassMs = 0.0f;
        uint32_t descriptorBindCount = 0;
        uint32_t chunkDescriptorBindCount = 0;
        uint32_t modelDescriptorBindCount = 0;
        uint32_t drawIndexedIndirectCount = 0;
        uint32_t drawIndexedCount = 0;
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
    void renderFrame(
        uint32_t windowWidth,
        uint32_t windowHeight,
        const glm::mat4 &viewMatrix,
        const glm::mat4 &projectionMatrix,
        const glm::mat4 &viewProjection,
        const FrameRenderData &frameData
    ) override;
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
    static constexpr uint32_t TIMESTAMP_QUERY_COUNT = 7;
    static constexpr uint32_t GI_PARAM_BINDING = 0;
    static constexpr uint32_t GI_SHADOW_OCCUPANCY_BINDING = 1;
    static constexpr uint32_t GI_MATERIAL_BINDING = 2;
    static constexpr uint32_t GI_NRD_DIFF_IN_BINDING = 3;
    static constexpr uint32_t GI_NRD_NORMAL_ROUGHNESS_IN_BINDING = 4;
    static constexpr uint32_t GI_NRD_MV_IN_BINDING = 5;
    static constexpr uint32_t GI_NRD_VIEWZ_IN_BINDING = 6;
    static constexpr uint32_t GI_NRD_DIFF_OUT_BINDING = 7;
    static constexpr uint32_t GI_NRD_COMPOSE_BASE_STORAGE_BINDING = 8;
    static constexpr uint32_t GI_NRD_COMPOSE_INDIRECT_STORAGE_BINDING = 9;
    static constexpr uint32_t GI_NRD_COMPOSE_BASE_SAMPLED_BINDING = 10;
    static constexpr uint32_t GI_NRD_COMPOSE_INDIRECT_SAMPLED_BINDING = 11;
    static constexpr uint32_t GI_NRD_SHADOW_IN_BINDING = 12;
    static constexpr uint32_t GI_NRD_SHADOW_OUT_BINDING = 13;
    static constexpr uint32_t GI_BLUE_NOISE_BINDING = 14;
    static constexpr uint32_t GI_RT_SCENE_BINDING = 15;
    static constexpr uint32_t GI_BINDING_COUNT = 16;

    VulkanContext &m_context;
    std::unique_ptr<NrdBootstrap> m_nrdBootstrap;
    bool m_initialized = false;

    vk::raii::CommandPool m_commandPool{nullptr};
    vk::raii::CommandPool m_nrdComputeCommandPool{nullptr};
    std::vector<vk::raii::CommandBuffer> m_commandBuffers;
    std::vector<vk::raii::CommandBuffer> m_compositeCommandBuffers;
    std::vector<vk::raii::CommandBuffer> m_nrdComputeCommandBuffers;
    std::vector<vk::raii::Framebuffer> m_framebuffers;
    std::vector<vk::raii::Framebuffer> m_compositeFramebuffers;
    std::vector<vk::raii::Semaphore> m_nrdGraphicsToComputeSemaphores;
    std::vector<vk::raii::Semaphore> m_nrdComputeToGraphicsSemaphores;
    bool m_nrdAsyncComputeEnabled = false;

    RenderPass m_renderPass;
    RenderPass m_compositeRenderPass;
    Pipeline m_chunkPipeline;
    Pipeline m_modelPipeline;
    FrameSync m_frameSync;
    UploadContext m_uploadContext;
    VkTexture m_fallbackArrayTexture;
    VkTexture m_fallback2DTexture;
    VkTexture m_giBlueNoiseTexture;

    struct PerImageDrawResources {
        vk::raii::Buffer modelMatrixBuffer{nullptr};
        vk::raii::DeviceMemory modelMatrixBufferMemory{nullptr};
        vk::DeviceSize modelMatrixCapacityBytes = 0;
        void *modelMatrixMapped = nullptr;

        vk::raii::Buffer indirectCommandBuffer{nullptr};
        vk::raii::DeviceMemory indirectCommandBufferMemory{nullptr};
        vk::DeviceSize indirectCommandCapacityBytes = 0;
        void *indirectCommandMapped = nullptr;

        vk::raii::Buffer chunkSuperbatchVertexBuffer{nullptr};
        vk::raii::DeviceMemory chunkSuperbatchVertexBufferMemory{nullptr};
        vk::DeviceSize chunkSuperbatchVertexCapacityBytes = 0;
        void *chunkSuperbatchVertexMapped = nullptr;

        vk::raii::Buffer chunkSuperbatchIndexBuffer{nullptr};
        vk::raii::DeviceMemory chunkSuperbatchIndexBufferMemory{nullptr};
        vk::DeviceSize chunkSuperbatchIndexCapacityBytes = 0;
        void *chunkSuperbatchIndexMapped = nullptr;

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
    vk::raii::Buffer m_giFallbackShadowOccupancyBuffer{nullptr};
    vk::raii::DeviceMemory m_giFallbackShadowOccupancyBufferMemory{nullptr};
    vk::raii::Buffer m_giFallbackMaterialBuffer{nullptr};
    vk::raii::DeviceMemory m_giFallbackMaterialBufferMemory{nullptr};
    struct SignalImageResources {
        vk::raii::Image image{nullptr};
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    glm::mat4 m_nrdPrevViewProjection{1.0f};
    glm::mat4 m_nrdPrevView{1.0f};
    glm::mat4 m_nrdPrevProjection{1.0f};
    bool m_nrdPrevMatricesValid = false;
    struct NrdPerImageResources {
        SignalImageResources diffIn;
        SignalImageResources normalRoughnessIn;
        SignalImageResources motionIn;
        SignalImageResources viewZIn;
        SignalImageResources diffOut;
        SignalImageResources shadowIn;
        SignalImageResources shadowOut;
        SignalImageResources composeBase;
        SignalImageResources composeIndirect;
    };
    std::vector<NrdPerImageResources> m_nrdPerImage;
    NrdPerImageResources m_nrdFallback{};
    bool m_nrdFallbackReady = false;
    bool m_loggedMissingNrdResources = false;
    std::vector<bool> m_nrdValidPerImage;
    vk::raii::Sampler m_nrdOutputSampler{nullptr};
    vk::raii::Sampler m_nrdNearestSampler{nullptr};
    vk::raii::Sampler m_nrdLinearSampler{nullptr};
    vk::raii::PipelineLayout m_postProcessPipelineLayout{nullptr};
    vk::raii::Pipeline m_postProcessPipeline{nullptr};
    vk::raii::PipelineLayout m_nrdCompositePipelineLayout{nullptr};
    vk::raii::Pipeline m_nrdCompositePipeline{nullptr};
#if VOXELOPS_NRD_HEADERS
    struct NrdRuntimeTexture {
        vk::Format format = vk::Format::eUndefined;
        uint32_t width = 0;
        uint32_t height = 0;
        SignalImageResources image;
    };
    struct NrdRuntimePerImage {
        std::vector<NrdRuntimeTexture> permanentPool;
        std::vector<NrdRuntimeTexture> transientPool;
    };
    struct NrdRuntimePerFrame {
        vk::raii::Buffer constantBuffer{nullptr};
        vk::raii::DeviceMemory constantMemory{nullptr};
        void *constantMapped = nullptr;
        vk::raii::DescriptorPool descriptorPool{nullptr};
        std::vector<vk::raii::DescriptorSet> resourcesSets;
        vk::raii::DescriptorSet constantsSet{nullptr};
    };
    std::vector<NrdRuntimePerImage> m_nrdRuntimePerImage;
    std::vector<NrdRuntimePerFrame> m_nrdRuntimePerFrame;
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
    uint32_t m_frameCounterLow = 0;
    std::vector<vk::raii::QueryPool> m_timestampQueryPools;
    bool m_timestampQueriesEnabled = false;
    float m_timestampPeriodNanoseconds = 0.0f;
    FrameTimingStats m_lastFrameTimingStats{};

    void createCommandPool();
    void createCommandBuffers();
    void createFramebuffers();
    void createCompositeFramebuffers();
    void createPostProcessPipeline();
    void cleanupPostProcessPipeline();
    void createNrdCompositePipeline();
    void cleanupNrdCompositePipeline();
    void createModelDescriptorResources();
    void createGiDescriptorResources();
    void updateGiHistoryAfterSubmit(
        const FrameRenderData &frameData,
        const glm::mat4 &viewMatrix,
        const glm::mat4 &projectionMatrix,
        const glm::mat4 &viewProjection,
        vk::Fence currentFrameFence
    );
    void createNrdSignalResources();
    void cleanupNrdSignalResources();

    template <typename T>
    void createPingPongImagePair(
        std::vector<std::array<T, 2>> &out,
        vk::Format format,
        vk::Extent2D extent,
        vk::ImageUsageFlags usage,
        const vk::ClearColorValue &clearValue,
        vk::raii::CommandBuffer &commandBuffer
    );

#if VOXELOPS_NRD_HEADERS
    bool createNrdRuntimeResources();
    void cleanupNrdRuntimeResources();
    void dispatchNrdPass(
        vk::CommandBuffer commandBuffer, uint32_t imageIndex, const FrameRenderData &frameData
    );
#endif

    void clearTemporalGiWriteTargets(uint32_t imageIndex);
    void barrierNrdSignalsForCompute(vk::CommandBuffer commandBuffer, uint32_t imageIndex);
    void barrierNrdSignalsForComposite(vk::CommandBuffer commandBuffer, uint32_t imageIndex);
    void recordNrdCompositePass(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const FrameRenderData &frameData,
        bool applyNrdComposite
    );
    void recordCompositeCommandBuffer(
        uint32_t imageIndex, const FrameRenderData &frameData, bool applyNrdComposite
    );
#if VOXELOPS_NRD_HEADERS
    void recordNrdComputeCommandBuffer(uint32_t imageIndex, const FrameRenderData &frameData);
#endif
    void createTimestampResources();
    void cleanupTimestampResources();
    void updateGpuTimingStatsForImage(uint32_t imageIndex);
    void ensurePerImageDrawBufferCapacity(
        uint32_t imageIndex, vk::DeviceSize modelBytes, vk::DeviceSize indirectBytes
    );
    void updatePerImageDrawBuffers(
        uint32_t imageIndex,
        const std::vector<glm::mat4> &modelMatrices,
        const std::vector<IndexedIndirectCommand> &indirectCommands,
        const std::vector<PackedVoxelVertexGpu> &chunkSuperbatchVertices,
        const std::vector<uint16_t> &chunkSuperbatchIndices
    );
    void updateModelDescriptorSet(uint32_t imageIndex);
    void updateGiDescriptorSet(
        uint32_t imageIndex, const FrameRenderData &frameData, const glm::mat4 &viewProjection
    );
    void cleanupPerImageDrawResources();
    void cleanupModelDescriptorResources();
    void cleanupGiDescriptorResources();

    void recreateSwapchainDependentResources();
    void cleanupSwapchainDependentResources();

    void recordCommandBuffer(
        uint32_t imageIndex,
        const glm::mat4 &viewProjection,
        const FrameRenderData &frameData,
        float &outChunkCpuMs,
        float &outModelCpuMs,
        float &outUiCpuMs,
        bool recordNrdAndComposite
    );
};
