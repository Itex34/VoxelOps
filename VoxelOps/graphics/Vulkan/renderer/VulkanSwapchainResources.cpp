#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"

#include <imgui.h>
#if __has_include(<imgui_impl_vulkan.h>)
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 1
#include <imgui_impl_vulkan.h>
#else
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 0
#endif

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    vk::raii::ShaderModule
    loadShaderModule(const vk::raii::Device &device, const std::string &path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("Failed to open shader: " + path);
        }

        const size_t size = static_cast<size_t>(file.tellg());
        if (size == 0 || (size % 4) != 0) {
            throw std::runtime_error("Invalid shader size: " + path);
        }

        std::vector<uint32_t> code(size / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(code.data()), static_cast<std::streamsize>(size));
        if (!file) {
            throw std::runtime_error("Failed to read shader: " + path);
        }

        vk::ShaderModuleCreateInfo shaderModuleInfo{};
        shaderModuleInfo.codeSize = size;
        shaderModuleInfo.pCode = code.data();
        return vk::raii::ShaderModule(device, shaderModuleInfo);
    }
} // namespace

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

    if (m_nrdPerImage.size() < swapchainImageViews.size()) {
        throw std::runtime_error("NRD per-image compose targets were not created for swapchain.");
    }
    for (size_t i = 0; i < swapchainImageViews.size(); ++i) {
        const auto &swapchainImageView = swapchainImageViews[i];
        vk::ImageView composeBaseView = *m_nrdPerImage[i].composeBase.view;
        vk::ImageView composeIndirectView = *m_nrdPerImage[i].composeIndirect.view;
        vk::ImageView nrdDiffInView = *m_nrdPerImage[i].diffIn.view;
        vk::ImageView nrdNormalRoughnessView = *m_nrdPerImage[i].normalRoughnessIn.view;
        vk::ImageView nrdMotionView = *m_nrdPerImage[i].motionIn.view;
        vk::ImageView nrdViewZView = *m_nrdPerImage[i].viewZIn.view;
        std::array<vk::ImageView, 8> attachments = {
            *swapchainImageView,
            composeBaseView,
            composeIndirectView,
            nrdDiffInView,
            nrdNormalRoughnessView,
            nrdMotionView,
            nrdViewZView,
            *m_context.getDepthImageView()
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

void VulkanRenderer::createCompositeFramebuffers() {
    const vk::raii::Device &device = m_context.getDevice();
    const auto &swapchainImageViews = m_context.getSwapchainImageViews();
    const vk::Extent2D extent = m_context.getSwapchainExtent();

    m_compositeFramebuffers.clear();
    m_compositeFramebuffers.reserve(swapchainImageViews.size());

    for (const auto &swapchainImageView : swapchainImageViews) {
        std::array<vk::ImageView, 1> attachments = {*swapchainImageView};

        vk::FramebufferCreateInfo framebufferInfo{};
        framebufferInfo.renderPass = *m_compositeRenderPass.get();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        m_compositeFramebuffers.emplace_back(device, framebufferInfo);
    }
}

void VulkanRenderer::createNrdCompositePipeline() {
    cleanupNrdCompositePipeline();

    if (m_compositeRenderPass.get() == nullptr || m_giDescriptorSetLayout == nullptr) {
        return;
    }

    std::string shaderDir;
#ifdef SHADER_DIR
    shaderDir = SHADER_DIR;
#endif
    if (!shaderDir.empty() && shaderDir.back() != '/' && shaderDir.back() != '\\') {
        shaderDir.push_back('/');
    }

    const vk::raii::Device &device = m_context.getDevice();
    vk::raii::ShaderModule vertShader =
        loadShaderModule(device, shaderDir + "nrd_composite.vert.spv");
    vk::raii::ShaderModule fragShader =
        loadShaderModule(device, shaderDir + "nrd_composite.frag.spv");

    vk::PipelineShaderStageCreateInfo vertStage{};
    vertStage.stage = vk::ShaderStageFlagBits::eVertex;
    vertStage.module = *vertShader;
    vertStage.pName = "main";

    vk::PipelineShaderStageCreateInfo fragStage{};
    fragStage.stage = vk::ShaderStageFlagBits::eFragment;
    fragStage.module = *fragShader;
    fragStage.pName = "main";

    std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages = {vertStage, fragStage};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};
    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    vk::PipelineViewportStateCreateInfo viewportState{};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    std::array<vk::DynamicState, 2> dynamicStates = {
        vk::DynamicState::eViewport, vk::DynamicState::eScissor
    };
    vk::PipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    vk::PipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = vk::PolygonMode::eFill;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = vk::CullModeFlagBits::eNone;
    rasterizer.frontFace = vk::FrontFace::eCounterClockwise;

    vk::PipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = vk::CompareOp::eAlways;

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<vk::DescriptorSetLayout, 1> setLayouts = {*m_giDescriptorSetLayout};
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    m_nrdCompositePipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

    vk::GraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = *m_nrdCompositePipelineLayout;
    pipelineInfo.renderPass = *m_compositeRenderPass.get();
    pipelineInfo.subpass = 0;

    m_nrdCompositePipeline = vk::raii::Pipeline(device, nullptr, pipelineInfo);
}

void VulkanRenderer::cleanupNrdCompositePipeline() {
    m_nrdCompositePipeline.clear();
    m_nrdCompositePipelineLayout.clear();
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

    constexpr vk::Format kComposeFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kNrdDiffFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kNrdMotionFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kNrdViewZFormat = vk::Format::eR32Sfloat;
    vk::Format nrdNormalRoughnessFormat = vk::Format::eA2B10G10R10UnormPack32;
    if (m_nrdBootstrap != nullptr) {
        switch (m_nrdBootstrap->normalEncoding()) {
        case 0u:
            nrdNormalRoughnessFormat = vk::Format::eR8G8B8A8Unorm;
            break;
        case 1u:
            nrdNormalRoughnessFormat = vk::Format::eR8G8B8A8Snorm;
            break;
        case 2u:
            nrdNormalRoughnessFormat = vk::Format::eA2B10G10R10UnormPack32;
            break;
        case 3u:
            nrdNormalRoughnessFormat = vk::Format::eR16G16B16A16Unorm;
            break;
        case 4u:
            nrdNormalRoughnessFormat = vk::Format::eR16G16B16A16Snorm;
            break;
        default:
            nrdNormalRoughnessFormat = vk::Format::eA2B10G10R10UnormPack32;
            break;
        }
    }
    const std::vector<vk::Format> gbufferFormats = {
        kComposeFormat,
        kComposeFormat,
        kNrdDiffFormat,
        nrdNormalRoughnessFormat,
        kNrdMotionFormat,
        kNrdViewZFormat
    };
    const uint32_t renderPassColorAttachmentCount = 1u + static_cast<uint32_t>(gbufferFormats.size());
    m_renderPass.create(
        device,
        m_context.getSwapchainImageFormat(),
        m_context.getDepthFormat(),
        true,
        vk::AttachmentLoadOp::eClear,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal,
        gbufferFormats
    );
    m_compositeRenderPass.create(
        device,
        m_context.getSwapchainImageFormat(),
        m_context.getDepthFormat(),
        false,
        vk::AttachmentLoadOp::eLoad,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR
    );
    createNrdSignalResources();
    createFramebuffers();
    createCompositeFramebuffers();
    createModelDescriptorResources();
    createGiDescriptorResources();
#if VOXELOPS_NRD_HEADERS
    createNrdRuntimeResources();
#endif
    const bool useRtShaderVariants = m_context.isHardwareRayTracingSupported();
    const char *chunkFragShader =
        useRtShaderVariants ? "VoxelTerrainGI_rt.frag.spv" : "VoxelTerrainGI.frag.spv";
    const char *modelFragShader = useRtShaderVariants ? "model_rt.frag.spv" : "model.frag.spv";
    m_chunkPipeline.create(
        device,
        m_renderPass.get(),
        m_fallbackArrayTexture.getDescriptorSetLayout(),
        m_modelDescriptorSetLayout,
        m_giDescriptorSetLayout,
        PipelineVertexLayout::PackedVoxel,
        "VoxelTerrainGI.vert.spv",
        chunkFragShader,
        renderPassColorAttachmentCount
    );
    m_modelPipeline.create(
        device,
        m_renderPass.get(),
        m_fallback2DTexture.getDescriptorSetLayout(),
        m_modelDescriptorSetLayout,
        m_giDescriptorSetLayout,
        PipelineVertexLayout::ModelPosUv,
        "model.vert.spv",
        modelFragShader,
        renderPassColorAttachmentCount
    );
    createNrdCompositePipeline();
    createCommandBuffers();
    createTimestampResources();
    m_frameSync.recreateSwapchainSync(device, m_framebuffers.size());

#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendRendererUserData != nullptr) {
        ImGui_ImplVulkan_SetMinImageCount(2);
        ImGui_ImplVulkan_PipelineInfo pipelineInfo{};
        pipelineInfo.RenderPass = *m_compositeRenderPass.get();
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
    m_compositeFramebuffers.clear();
    cleanupNrdCompositePipeline();
    m_chunkPipeline.cleanup();
    m_modelPipeline.cleanup();
    cleanupPerImageDrawResources();
    cleanupModelDescriptorResources();
    cleanupGiDescriptorResources();
#if VOXELOPS_NRD_HEADERS
    cleanupNrdRuntimeResources();
#endif
    cleanupNrdSignalResources();
    m_renderPass.cleanup();
    m_compositeRenderPass.cleanup();
}
