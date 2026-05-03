#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

template <typename T>
void VulkanRenderer::createPingPongImagePair(
    std::vector<std::array<T, 2>> &out,
    vk::Format format,
    vk::Extent2D extent,
    vk::ImageUsageFlags usage,
    const vk::ClearColorValue &clearValue,
    vk::raii::CommandBuffer &commandBuffer
) {
    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();

    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    for (auto &pair : out) {
        for (auto &res : pair) {
            vk::ImageCreateInfo imageInfo{};
            imageInfo.imageType = vk::ImageType::e2D;
            imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1u);
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = format;
            imageInfo.tiling = vk::ImageTiling::eOptimal;
            imageInfo.initialLayout = vk::ImageLayout::eUndefined;
            imageInfo.usage = usage;
            imageInfo.samples = vk::SampleCountFlagBits::e1;
            imageInfo.sharingMode = vk::SharingMode::eExclusive;
            res.image = vk::raii::Image(device, imageInfo);

            const vk::MemoryRequirements requirements = res.image.getMemoryRequirements();
            vk::MemoryAllocateInfo allocInfo{};
            allocInfo.allocationSize = requirements.size;
            allocInfo.memoryTypeIndex = VulkanUtils::findMemoryType(
                physicalDevice,
                requirements.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eDeviceLocal
            );
            res.memory = vk::raii::DeviceMemory(device, allocInfo);
            res.image.bindMemory(*res.memory, 0);

            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = *res.image;
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = range;
            viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            res.view = vk::raii::ImageView(device, viewInfo);

            vk::ImageMemoryBarrier toGeneral{};
            toGeneral.oldLayout = vk::ImageLayout::eUndefined;
            toGeneral.newLayout = vk::ImageLayout::eGeneral;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = *res.image;
            toGeneral.subresourceRange = range;
            toGeneral.srcAccessMask = {};
            toGeneral.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                      vk::AccessFlagBits::eShaderWrite |
                                      vk::AccessFlagBits::eTransferWrite;
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader |
                    vk::PipelineStageFlagBits::eComputeShader,
                {},
                {},
                {},
                toGeneral
            );
            commandBuffer.clearColorImage(*res.image, vk::ImageLayout::eGeneral, clearValue, range);
        }
    }
}

vk::raii::Sampler VulkanRenderer::createSharedRestirSampler() {
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    return vk::raii::Sampler(m_context.getDevice(), samplerInfo);
}

void VulkanRenderer::createRestirDiResources() {
    cleanupRestirDiResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const size_t imageCount = m_framebuffers.size();
    m_restirDiPerImage.resize(imageCount);
    m_restirDiWriteParityPerImage.assign(imageCount, 0u);
    m_restirDiValidPerImage.assign(imageCount, false);
    m_restirDiPrevViewProjectionPerImage.assign(imageCount, glm::mat4(1.0f));
    m_restirDiPrevViewPerImage.assign(imageCount, glm::mat4(1.0f));
    m_restirDiPrevProjectionPerImage.assign(imageCount, glm::mat4(1.0f));
    m_prevViewProjectionValidPerImage.assign(imageCount, false);

    m_restirDiSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(
        m_restirDiPerImage,
        vk::Format::eR16G16B16A16Sfloat,
        extent,
        kUsage,
        vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
        cmd
    );
    VulkanUtils::endSingleTimeCommands(
        m_context.getDevice(), m_context.getGraphicsQueue(), std::move(cmd)
    );
}

void VulkanRenderer::cleanupRestirDiResources() {
    m_restirDiSampler.clear();
    m_restirDiPerImage.clear();
    m_restirDiWriteParityPerImage.clear();
    m_restirDiValidPerImage.clear();
    m_restirDiPrevViewProjectionPerImage.clear();
    m_restirDiPrevViewPerImage.clear();
    m_restirDiPrevProjectionPerImage.clear();
    m_prevViewProjectionValidPerImage.clear();
}

void VulkanRenderer::createRestirValidationResources() {
    cleanupRestirValidationResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    m_restirValidationPerImage.resize(m_framebuffers.size());
    m_restirValidationSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(
        m_restirValidationPerImage,
        vk::Format::eR16G16B16A16Sfloat,
        extent,
        kUsage,
        vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.0f}),
        cmd
    );
    VulkanUtils::endSingleTimeCommands(
        m_context.getDevice(), m_context.getGraphicsQueue(), std::move(cmd)
    );
}

void VulkanRenderer::cleanupRestirValidationResources() {
    m_restirValidationSampler.clear();
    m_restirValidationPerImage.clear();
}

void VulkanRenderer::createRestirMetaResources() {
    cleanupRestirMetaResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    m_restirMetaPerImage.resize(m_framebuffers.size());
    m_restirMetaSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(
        m_restirMetaPerImage,
        vk::Format::eR16G16B16A16Sfloat,
        extent,
        kUsage,
        vk::ClearColorValue(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}),
        cmd
    );
    VulkanUtils::endSingleTimeCommands(
        m_context.getDevice(), m_context.getGraphicsQueue(), std::move(cmd)
    );
}

void VulkanRenderer::cleanupRestirMetaResources() {
    m_restirMetaSampler.clear();
    m_restirMetaPerImage.clear();
}

void VulkanRenderer::createRestirGiResources() {
    cleanupRestirGiResources();
    if (m_framebuffers.empty() || m_commandPool == nullptr)
        return;

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const size_t imageCount = m_framebuffers.size();
    m_restirGiPerImage.resize(imageCount);
    m_restirGiMetaPerImage.resize(imageCount);
    m_restirGiSampler = createSharedRestirSampler();
    m_restirGiMetaSampler = createSharedRestirSampler();

    constexpr vk::ImageUsageFlags kUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst;

    // Both pairs go into one command buffer for a single queue submission.
    vk::raii::CommandBuffer cmd =
        VulkanUtils::beginSingleTimeCommands(m_context.getDevice(), m_commandPool);
    createPingPongImagePair(
        m_restirGiPerImage,
        vk::Format::eR16G16B16A16Sfloat,
        extent,
        kUsage,
        vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
        cmd
    );
    createPingPongImagePair(
        m_restirGiMetaPerImage,
        vk::Format::eR16G16B16A16Sfloat,
        extent,
        kUsage,
        vk::ClearColorValue(std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}),
        cmd
    );
    VulkanUtils::endSingleTimeCommands(
        m_context.getDevice(), m_context.getGraphicsQueue(), std::move(cmd)
    );
}

void VulkanRenderer::cleanupRestirGiResources() {
    m_restirGiSampler.clear();
    m_restirGiMetaSampler.clear();
    m_restirGiPerImage.clear();
    m_restirGiMetaPerImage.clear();
}
