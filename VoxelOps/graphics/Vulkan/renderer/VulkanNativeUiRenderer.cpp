#include "VulkanNativeUiRenderer.hpp"

#include "../vulkan/UploadContext.hpp"
#include "../vulkan/VulkanContext.hpp"
#include "../vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
    constexpr NativeUiTextureHandle kFallbackTextureHandle = 0;
    constexpr uint32_t kDefaultMaxUiTextures = 64;
    constexpr vk::DeviceSize kMinVertexBufferBytes = 4096;
    constexpr vk::DeviceSize kMinIndexBufferBytes = 4096;

    vk::DeviceSize GrowCapacity(vk::DeviceSize minimum, vk::DeviceSize required) {
        vk::DeviceSize capacity = minimum;
        while (capacity < required) {
            capacity *= 2;
        }
        return capacity;
    }

    vk::Format ToVkFormat(NativeUiTextureFormat format) {
        switch (format) {
        case NativeUiTextureFormat::Rgba8:
            return vk::Format::eR8G8B8A8Unorm;
        case NativeUiTextureFormat::R8:
        default:
            return vk::Format::eR8Unorm;
        }
    }

    uint32_t BytesPerPixel(NativeUiTextureFormat format) {
        switch (format) {
        case NativeUiTextureFormat::Rgba8:
            return 4;
        case NativeUiTextureFormat::R8:
        default:
            return 1;
        }
    }

    bool ComputeScissor(NativeUiClipRect clip, vk::Extent2D framebufferExtent, vk::Rect2D &out) {
        const float x0 = std::max(0.0f, clip.x);
        const float y0 = std::max(0.0f, clip.y);
        const float x1 = std::min(static_cast<float>(framebufferExtent.width), clip.x + clip.w);
        const float y1 = std::min(static_cast<float>(framebufferExtent.height), clip.y + clip.h);

        const int32_t left = static_cast<int32_t>(std::floor(x0));
        const int32_t top = static_cast<int32_t>(std::floor(y0));
        const int32_t right = static_cast<int32_t>(std::ceil(x1));
        const int32_t bottom = static_cast<int32_t>(std::ceil(y1));
        if (right <= left || bottom <= top) {
            return false;
        }

        out.offset = vk::Offset2D{left, top};
        out.extent = vk::Extent2D{
            static_cast<uint32_t>(right - left),
            static_cast<uint32_t>(bottom - top)
        };
        return out.extent.width > 0 && out.extent.height > 0;
    }
}

void VulkanNativeUiRenderer::initialize(
    VulkanContext &context,
    UploadContext &uploadContext,
    vk::RenderPass renderPass,
    uint32_t swapchainImageCount
) {
    cleanup();
    m_context = &context;
    m_uploadContext = &uploadContext;

    createDescriptorResources(kDefaultMaxUiTextures);
    createPipeline(renderPass);
    m_perImage.resize(swapchainImageCount);
    createFallbackTexture();
    m_initialized = true;
}

void VulkanNativeUiRenderer::cleanup() {
    for (PerImageResources &resources : m_perImage) {
        if (resources.vertexMapped != nullptr) {
            VulkanUtils::unmapAllocation(resources.vertexBuffer);
            resources.vertexMapped = nullptr;
        }
        if (resources.indexMapped != nullptr) {
            VulkanUtils::unmapAllocation(resources.indexBuffer);
            resources.indexMapped = nullptr;
        }
        VulkanUtils::destroyBuffer(resources.vertexBuffer);
        VulkanUtils::destroyBuffer(resources.indexBuffer);
        resources.vertexCapacityBytes = 0;
        resources.indexCapacityBytes = 0;
    }
    m_perImage.clear();
    for (auto &[_, texture] : m_textures) {
        texture.view.clear();
        texture.sampler.clear();
        texture.descriptorSet.clear();
        VulkanUtils::destroyImage(texture.image);
    }
    m_textures.clear();
    m_pipeline.clear();
    m_pipelineLayout.clear();
    m_descriptorPool.clear();
    m_descriptorSetLayout.clear();
    m_context = nullptr;
    m_uploadContext = nullptr;
    m_initialized = false;
}

void VulkanNativeUiRenderer::render(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const NativeUiDrawData &drawData,
    vk::Extent2D extent
) {
    if (!m_initialized || imageIndex >= m_perImage.size() || drawData.indexCount == 0 ||
        drawData.batchCount == 0 || drawData.vertices == nullptr || drawData.indices == nullptr) {
        return;
    }

    for (size_t i = 0; i < drawData.textureUploadCount; ++i) {
        ensureTexture(drawData.textureUploads[i]);
    }

    const vk::DeviceSize vertexBytes =
        static_cast<vk::DeviceSize>(drawData.vertexCount * sizeof(NativeUiVertex));
    const vk::DeviceSize indexBytes =
        static_cast<vk::DeviceSize>(drawData.indexCount * sizeof(uint32_t));
    ensureBufferCapacity(imageIndex, vertexBytes, indexBytes);

    PerImageResources &resources = m_perImage[imageIndex];
    if (resources.vertexMapped == nullptr || resources.indexMapped == nullptr) {
        return;
    }

    std::memcpy(resources.vertexMapped, drawData.vertices, static_cast<size_t>(vertexBytes));
    std::memcpy(resources.indexMapped, drawData.indices, static_cast<size_t>(indexBytes));

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_pipeline);

    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    commandBuffer.setViewport(0, viewport);

    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{0, 0};
    scissor.extent = extent;

    const std::array<vk::Buffer, 1> vertexBuffers = {*resources.vertexBuffer};
    const std::array<vk::DeviceSize, 1> vertexOffsets = {0};
    commandBuffer.bindVertexBuffers(0, vertexBuffers, vertexOffsets);
    commandBuffer.bindIndexBuffer(*resources.indexBuffer, 0, vk::IndexType::eUint32);

    for (size_t i = 0; i < drawData.batchCount; ++i) {
        const NativeUiDrawBatch &batch = drawData.batches[i];
        if (batch.clipEnabled) {
            vk::Rect2D clippedScissor{};
            if (!ComputeScissor(batch.clip, extent, clippedScissor)) {
                continue;
            }
            commandBuffer.setScissor(0, clippedScissor);
        } else {
            commandBuffer.setScissor(0, scissor);
        }

        const auto textureIt = m_textures.find(batch.texture);
        const TextureResource *texture = nullptr;
        if (textureIt != m_textures.end()) {
            texture = &textureIt->second;
        } else {
            const auto fallbackIt = m_textures.find(kFallbackTextureHandle);
            if (fallbackIt != m_textures.end()) {
                texture = &fallbackIt->second;
            }
        }
        if (texture == nullptr || texture->descriptorSet == nullptr) {
            continue;
        }

        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            *m_pipelineLayout,
            0,
            *texture->descriptorSet,
            {}
        );

        PushConstants push{};
        push.screenSize[0] = static_cast<float>(std::max(drawData.width, 1));
        push.screenSize[1] = static_cast<float>(std::max(drawData.height, 1));
        push.textureMode = static_cast<uint32_t>(batch.textureMode);
        commandBuffer.pushConstants<PushConstants>(
            *m_pipelineLayout,
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            0,
            push
        );

        commandBuffer.drawIndexed(batch.indexCount, 1, batch.indexOffset, 0, 0);
    }
}

void VulkanNativeUiRenderer::createPipeline(vk::RenderPass renderPass) {
    std::string shaderDir;
#ifdef SHADER_DIR
    shaderDir = SHADER_DIR;
#endif
    if (!shaderDir.empty() && shaderDir.back() != '/' && shaderDir.back() != '\\') {
        shaderDir.push_back('/');
    }

    const vk::raii::Device &device = m_context->getDevice();
    vk::raii::ShaderModule vertShader =
        VulkanUtils::loadShaderModule(device, shaderDir + "native_ui.vert.spv");
    vk::raii::ShaderModule fragShader =
        VulkanUtils::loadShaderModule(device, shaderDir + "native_ui.frag.spv");

    vk::PipelineShaderStageCreateInfo vertStage{};
    vertStage.stage = vk::ShaderStageFlagBits::eVertex;
    vertStage.module = *vertShader;
    vertStage.pName = "main";

    vk::PipelineShaderStageCreateInfo fragStage{};
    fragStage.stage = vk::ShaderStageFlagBits::eFragment;
    fragStage.module = *fragShader;
    fragStage.pName = "main";

    const std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages = {vertStage, fragStage};

    vk::VertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(NativeUiVertex);
    binding.inputRate = vk::VertexInputRate::eVertex;

    std::array<vk::VertexInputAttributeDescription, 3> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = vk::Format::eR32G32Sfloat;
    attributes[0].offset = offsetof(NativeUiVertex, x);
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = vk::Format::eR32G32Sfloat;
    attributes[1].offset = offsetof(NativeUiVertex, u);
    attributes[2].binding = 0;
    attributes[2].location = 2;
    attributes[2].format = vk::Format::eR32G32B32A32Sfloat;
    attributes[2].offset = offsetof(NativeUiVertex, r);

    vk::PipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    vk::PipelineViewportStateCreateInfo viewportState{};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const std::array<vk::DynamicState, 2> dynamicStates = {
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

    vk::PipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    blendAttachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.logicOpEnable = VK_FALSE;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    const vk::DescriptorSetLayout setLayout = *m_descriptorSetLayout;
    vk::PipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    m_pipelineLayout = vk::raii::PipelineLayout(device, layoutInfo);

    vk::GraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = *m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    m_pipeline = vk::raii::Pipeline(device, nullptr, pipelineInfo);
}

void VulkanNativeUiRenderer::createDescriptorResources(uint32_t maxTextureCount) {
    const vk::raii::Device &device = m_context->getDevice();

    vk::DescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;
    m_descriptorSetLayout = vk::raii::DescriptorSetLayout(device, layoutInfo);

    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eCombinedImageSampler;
    poolSize.descriptorCount = maxTextureCount;

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = maxTextureCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    m_descriptorPool = vk::raii::DescriptorPool(device, poolInfo);
}

void VulkanNativeUiRenderer::ensureTexture(const NativeUiTextureUpload &upload) {
    if (upload.handle == 0 || upload.pixels == nullptr || upload.width <= 0 || upload.height <= 0) {
        return;
    }
    const auto it = m_textures.find(upload.handle);
    if (it != m_textures.end() && it->second.width == upload.width &&
        it->second.height == upload.height && it->second.format == upload.format) {
        return;
    }
    createTexture(upload.handle, upload);
}

void VulkanNativeUiRenderer::createTexture(
    NativeUiTextureHandle handle,
    const NativeUiTextureUpload &upload
) {
    const vk::raii::Device &device = m_context->getDevice();
    if (const auto existing = m_textures.find(handle); existing != m_textures.end()) {
        existing->second.view.clear();
        existing->second.sampler.clear();
        existing->second.descriptorSet.clear();
        VulkanUtils::destroyImage(existing->second.image);
        m_textures.erase(existing);
    }

    TextureResource resource{};
    resource.format = upload.format;
    resource.width = upload.width;
    resource.height = upload.height;

    const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(
        static_cast<size_t>(upload.width) * static_cast<size_t>(upload.height) *
        BytesPerPixel(upload.format)
    );
    UploadContext::StagingBuffer staging =
        m_uploadContext->createStagingBuffer(upload.pixels, imageSize);

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent = vk::Extent3D(
        static_cast<uint32_t>(upload.width), static_cast<uint32_t>(upload.height), 1
    );
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = ToVkFormat(upload.format);
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    VulkanUtils::createImage(
        m_context->getVmaAllocator(),
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        resource.image
    );

    m_uploadContext->submitImageUpload(
        std::move(staging), *resource.image, static_cast<uint32_t>(upload.width), static_cast<uint32_t>(upload.height)
    );

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = *resource.image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = ToVkFormat(upload.format);
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    resource.view = vk::raii::ImageView(device, viewInfo);

    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = vk::BorderColor::eIntOpaqueWhite;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    resource.sampler = vk::raii::Sampler(device, samplerInfo);

    vk::DescriptorSetLayout setLayout = *m_descriptorSetLayout;
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    std::vector<vk::raii::DescriptorSet> descriptorSets = device.allocateDescriptorSets(allocateInfo);
    if (descriptorSets.empty()) {
        throw std::runtime_error("Failed to allocate native UI descriptor set.");
    }
    resource.descriptorSet = std::move(descriptorSets[0]);

    vk::DescriptorImageInfo imageDescriptor{};
    imageDescriptor.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    imageDescriptor.imageView = *resource.view;
    imageDescriptor.sampler = *resource.sampler;

    vk::WriteDescriptorSet write{};
    write.dstSet = *resource.descriptorSet;
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &imageDescriptor;
    device.updateDescriptorSets(std::array<vk::WriteDescriptorSet, 1>{write}, {});

    m_textures[handle] = std::move(resource);
}

void VulkanNativeUiRenderer::createFallbackTexture() {
    const uint32_t white = 0xffffffffu;
    NativeUiTextureUpload upload{};
    upload.handle = kFallbackTextureHandle;
    upload.format = NativeUiTextureFormat::Rgba8;
    upload.width = 1;
    upload.height = 1;
    upload.pixels = &white;
    upload.byteCount = sizeof(white);
    createTexture(kFallbackTextureHandle, upload);
}

void VulkanNativeUiRenderer::ensureBufferCapacity(
    uint32_t imageIndex,
    vk::DeviceSize vertexBytes,
    vk::DeviceSize indexBytes
) {
    if (imageIndex >= m_perImage.size()) {
        return;
    }
    PerImageResources &resources = m_perImage[imageIndex];
    const VmaAllocator allocator = m_context->getVmaAllocator();

    if (resources.vertexBuffer == nullptr || resources.vertexCapacityBytes < vertexBytes) {
        if (resources.vertexMapped != nullptr) {
            VulkanUtils::unmapAllocation(resources.vertexBuffer);
            resources.vertexMapped = nullptr;
        }
        VulkanUtils::destroyBuffer(resources.vertexBuffer);
        resources.vertexCapacityBytes = GrowCapacity(kMinVertexBufferBytes, vertexBytes);
        VulkanUtils::createBuffer(
            allocator,
            resources.vertexCapacityBytes,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            resources.vertexBuffer
        );
        resources.vertexMapped = VulkanUtils::mapAllocation(resources.vertexBuffer);
    }

    if (resources.indexBuffer == nullptr || resources.indexCapacityBytes < indexBytes) {
        if (resources.indexMapped != nullptr) {
            VulkanUtils::unmapAllocation(resources.indexBuffer);
            resources.indexMapped = nullptr;
        }
        VulkanUtils::destroyBuffer(resources.indexBuffer);
        resources.indexCapacityBytes = GrowCapacity(kMinIndexBufferBytes, indexBytes);
        VulkanUtils::createBuffer(
            allocator,
            resources.indexCapacityBytes,
            vk::BufferUsageFlagBits::eIndexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            resources.indexBuffer
        );
        resources.indexMapped = VulkanUtils::mapAllocation(resources.indexBuffer);
    }
}
