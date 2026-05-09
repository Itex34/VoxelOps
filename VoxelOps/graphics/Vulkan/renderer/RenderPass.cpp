#include "graphics/Vulkan/renderer/RenderPass.hpp"

#include <array>
#include <vector>

void RenderPass::create(
    const vk::raii::Device &device,
    vk::Format colorFormat,
    vk::Format depthFormat,
    bool withDepth,
    vk::AttachmentLoadOp colorLoadOp,
    vk::ImageLayout colorInitialLayout,
    vk::ImageLayout colorFinalLayout,
    const std::vector<vk::Format> &extraColorFormats
) {
    std::vector<vk::AttachmentDescription> attachments;
    attachments.reserve(1u + extraColorFormats.size() + (withDepth ? 1u : 0u));

    vk::AttachmentDescription colorAttachment{};
    colorAttachment.format = colorFormat;
    colorAttachment.samples = vk::SampleCountFlagBits::e1;
    colorAttachment.loadOp = colorLoadOp;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    colorAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    colorAttachment.initialLayout = colorInitialLayout;
    colorAttachment.finalLayout = colorFinalLayout;
    attachments.push_back(colorAttachment);

    for (vk::Format format : extraColorFormats) {
        vk::AttachmentDescription extra{};
        extra.format = format;
        extra.samples = vk::SampleCountFlagBits::e1;
        extra.loadOp = vk::AttachmentLoadOp::eClear;
        extra.storeOp = vk::AttachmentStoreOp::eStore;
        extra.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        extra.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        extra.initialLayout = vk::ImageLayout::eUndefined;
        extra.finalLayout = vk::ImageLayout::eGeneral;
        attachments.push_back(extra);
    }

    vk::AttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = vk::SampleCountFlagBits::e1;
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
    depthAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    depthAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    depthAttachment.initialLayout = vk::ImageLayout::eUndefined;
    depthAttachment.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    std::vector<vk::AttachmentReference> colorAttachmentRefs;
    colorAttachmentRefs.reserve(1u + extraColorFormats.size());
    colorAttachmentRefs.push_back(vk::AttachmentReference{0u, vk::ImageLayout::eColorAttachmentOptimal});
    for (uint32_t i = 0; i < static_cast<uint32_t>(extraColorFormats.size()); ++i) {
        colorAttachmentRefs.push_back(
            vk::AttachmentReference{1u + i, vk::ImageLayout::eColorAttachmentOptimal}
        );
    }

    vk::AttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = static_cast<uint32_t>(1u + extraColorFormats.size());
    depthAttachmentRef.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentRefs.size());
    subpass.pColorAttachments = colorAttachmentRefs.data();
    subpass.pDepthStencilAttachment = withDepth ? &depthAttachmentRef : nullptr;

    vk::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                               vk::AccessFlagBits::eColorAttachmentWrite;
    if (colorInitialLayout == vk::ImageLayout::eUndefined ||
        colorLoadOp == vk::AttachmentLoadOp::eClear) {
        dependency.srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
        dependency.srcAccessMask = {};
    }
    if (withDepth) {
        dependency.srcStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests;
        dependency.dstStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests;
        dependency.dstAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    }

    if (withDepth) {
        attachments.push_back(depthAttachment);
    }

    vk::RenderPassCreateInfo renderPassInfo{};
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    m_renderPass = vk::raii::RenderPass(device, renderPassInfo);
}

void RenderPass::cleanup() {
    m_renderPass.clear();
}
