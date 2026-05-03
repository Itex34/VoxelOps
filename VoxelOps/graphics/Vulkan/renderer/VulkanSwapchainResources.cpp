#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/vulkan/VulkanContext.hpp"

#include <imgui.h>
#if __has_include(<imgui_impl_vulkan.h>)
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 1
#include <imgui_impl_vulkan.h>
#else
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 0
#endif

void VulkanRenderer::createCommandBuffers() {
    const vk::raii::Device &device = m_context.getDevice();
    m_commandBuffers.clear();

    if (m_framebuffers.empty()) {
        return;
    }

    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = *m_commandPool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_framebuffers.size());
    m_commandBuffers = device.allocateCommandBuffers(allocInfo);
}

void VulkanRenderer::createFramebuffers() {
    const vk::raii::Device &device = m_context.getDevice();
    const auto &swapchainImageViews = m_context.getSwapchainImageViews();
    const vk::Extent2D extent = m_context.getSwapchainExtent();

    m_framebuffers.clear();
    m_framebuffers.reserve(swapchainImageViews.size());

    for (const auto &swapchainImageView : swapchainImageViews) {
        std::array<vk::ImageView, 2> attachments = {
            *swapchainImageView, *m_context.getDepthImageView()
        };

        vk::FramebufferCreateInfo framebufferInfo{};
        framebufferInfo.renderPass = *m_renderPass.get();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        m_framebuffers.emplace_back(device, framebufferInfo);
    }
}

void VulkanRenderer::createTimestampResources() {
    cleanupTimestampResources();

    m_timestampQueriesEnabled = m_context.areTimestampQueriesSupported();
    m_timestampPeriodNanoseconds = m_context.getTimestampPeriodNanoseconds();
    if (!m_timestampQueriesEnabled || m_timestampPeriodNanoseconds <= 0.0f ||
        m_framebuffers.empty()) {
        m_timestampQueriesEnabled = false;
        m_timestampPeriodNanoseconds = 0.0f;
        return;
    }

    const vk::raii::Device &device = m_context.getDevice();
    m_timestampQueryPools.reserve(m_framebuffers.size());
    for (size_t i = 0; i < m_framebuffers.size(); ++i) {
        vk::QueryPoolCreateInfo queryPoolInfo{};
        queryPoolInfo.queryType = vk::QueryType::eTimestamp;
        queryPoolInfo.queryCount = TIMESTAMP_QUERY_COUNT;
        m_timestampQueryPools.emplace_back(device, queryPoolInfo);
    }
}

void VulkanRenderer::cleanupTimestampResources() {
    m_timestampQueryPools.clear();
    m_timestampQueriesEnabled = false;
    m_timestampPeriodNanoseconds = 0.0f;
    m_lastFrameTimingStats.gpuValid = false;
}

void VulkanRenderer::recreateSwapchainDependentResources() {
    cleanupSwapchainDependentResources();

    const vk::raii::Device &device = m_context.getDevice();

    m_renderPass.create(device, m_context.getSwapchainImageFormat(), m_context.getDepthFormat());
    createFramebuffers();
    createModelDescriptorResources();
    createGiDescriptorResources();
    createRestirDiResources();
    createRestirValidationResources();
    createRestirMetaResources();
    createRestirGiResources();
    createNrdSignalResources();
#if VOXELOPS_NRD_HEADERS
    createNrdRuntimeResources();
#endif
    createRestirGiSpatialResources();
    const bool useRtShaderVariants = m_context.isHardwareRayTracingSupported();
    const char *chunkFragShader =
        useRtShaderVariants ? "triangle_rt.frag.spv" : "triangle.frag.spv";
    const char *modelFragShader = useRtShaderVariants ? "model_rt.frag.spv" : "model.frag.spv";
    m_chunkPipeline.create(
        device,
        m_renderPass.get(),
        m_fallbackArrayTexture.getDescriptorSetLayout(),
        m_modelDescriptorSetLayout,
        m_giDescriptorSetLayout,
        PipelineVertexLayout::PackedVoxel,
        "triangle.vert.spv",
        chunkFragShader
    );
    m_modelPipeline.create(
        device,
        m_renderPass.get(),
        m_fallback2DTexture.getDescriptorSetLayout(),
        m_modelDescriptorSetLayout,
        m_giDescriptorSetLayout,
        PipelineVertexLayout::ModelPosUv,
        "model.vert.spv",
        modelFragShader
    );
    createCommandBuffers();
    createTimestampResources();
    m_frameSync.recreateSwapchainSync(device, m_framebuffers.size());

#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_SetMinImageCount(2);
        ImGui_ImplVulkan_PipelineInfo pipelineInfo{};
        pipelineInfo.RenderPass = *m_renderPass.get();
        pipelineInfo.Subpass = 0;
        pipelineInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_CreateMainPipeline(&pipelineInfo);
    }
#endif
}

void VulkanRenderer::cleanupSwapchainDependentResources() {
    cleanupTimestampResources();
    m_commandBuffers.clear();
    m_framebuffers.clear();
    m_chunkPipeline.cleanup();
    m_modelPipeline.cleanup();
    cleanupPerImageDrawResources();
    cleanupModelDescriptorResources();
    cleanupGiDescriptorResources();
    cleanupRestirDiResources();
    cleanupRestirValidationResources();
    cleanupRestirMetaResources();
    cleanupRestirGiResources();
#if VOXELOPS_NRD_HEADERS
    cleanupNrdRuntimeResources();
#endif
    cleanupNrdSignalResources();
    cleanupRestirGiSpatialResources();
    m_renderPass.cleanup();
}
