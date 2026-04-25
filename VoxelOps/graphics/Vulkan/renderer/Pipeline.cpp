#include "graphics/Vulkan/renderer/Pipeline.hpp"

#include "graphics/Vulkan/graphics/Mesh.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
vk::raii::ShaderModule loadShaderModule(const vk::raii::Device &device, const std::string &path) {
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

void Pipeline::create(const vk::raii::Device &device, const vk::raii::RenderPass &renderPass,
                      const vk::raii::DescriptorSetLayout &textureDescriptorSetLayout,
                      const vk::raii::DescriptorSetLayout &modelDescriptorSetLayout,
                      const vk::raii::DescriptorSetLayout &giDescriptorSetLayout,
                      PipelineVertexLayout vertexLayout, const char *vertexShaderFile,
                      const char *fragmentShaderFile) {
    std::string shaderDir;
#ifdef SHADER_DIR
    shaderDir = SHADER_DIR;
#endif
    if (!shaderDir.empty() && shaderDir.back() != '/' && shaderDir.back() != '\\') {
        shaderDir.push_back('/');
    }

    vk::raii::ShaderModule vertShader = loadShaderModule(device, shaderDir + vertexShaderFile);
    vk::raii::ShaderModule fragShader = loadShaderModule(device, shaderDir + fragmentShaderFile);

    vk::PipelineShaderStageCreateInfo vertStage{};
    vertStage.stage = vk::ShaderStageFlagBits::eVertex;
    vertStage.module = *vertShader;
    vertStage.pName = "main";

    vk::PipelineShaderStageCreateInfo fragStage{};
    fragStage.stage = vk::ShaderStageFlagBits::eFragment;
    fragStage.module = *fragShader;
    fragStage.pName = "main";

    std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages = {vertStage, fragStage};

    vk::VertexInputBindingDescription bindingDescription{};
    std::array<vk::VertexInputAttributeDescription, 2> attributeDescriptions{};
    if (vertexLayout == PipelineVertexLayout::PackedVoxel) {
        bindingDescription = VkMesh::PackedVoxelVertex::getBindingDescription();
        attributeDescriptions = VkMesh::PackedVoxelVertex::getAttributeDescriptions();
    } else {
        bindingDescription = VkMesh::Vertex::getBindingDescription();
        attributeDescriptions = VkMesh::Vertex::getAttributeDescriptions();
    }

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    vk::PipelineViewportStateCreateInfo viewportState{};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    std::array<vk::DynamicState, 2> dynamicStates = {vk::DynamicState::eViewport,
                                                     vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    vk::PipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = vk::PolygonMode::eFill;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = vk::CullModeFlagBits::eBack;
    rasterizer.frontFace = vk::FrontFace::eCounterClockwise;

    vk::PipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = vk::CompareOp::eLess;

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    vk::PushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eVertex;
    pushConstantRange.size = static_cast<uint32_t>(sizeof(PushConstants));

    const std::array<vk::DescriptorSetLayout, 3> setLayouts = {
        *textureDescriptorSetLayout, *modelDescriptorSetLayout, *giDescriptorSetLayout};
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    m_pipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

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
    pipelineInfo.layout = *m_pipelineLayout;
    pipelineInfo.renderPass = *renderPass;
    pipelineInfo.subpass = 0;

    m_graphicsPipeline = vk::raii::Pipeline(device, nullptr, pipelineInfo);
}

void Pipeline::cleanup() {
    m_graphicsPipeline.clear();
    m_pipelineLayout.clear();
}

void Pipeline::bind(const vk::raii::CommandBuffer &commandBuffer) const {
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_graphicsPipeline);
}

void Pipeline::pushViewProjection(const vk::raii::CommandBuffer &commandBuffer,
                                  const glm::mat4 &viewProjection) const {
    PushConstants pushConstants{};
    pushConstants.viewProjection = viewProjection;

    commandBuffer.pushConstants<PushConstants>(*m_pipelineLayout, vk::ShaderStageFlagBits::eVertex,
                                               0, pushConstants);
}
