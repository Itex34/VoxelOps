#include "VulkanRenderer.hpp"
#include "NrdBootstrap.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace {
    vk::ClearColorValue nrdNormalRoughnessClearValue(uint32_t normalEncoding) {
        if (normalEncoding == 2u) {
            // Packed oct normal for +Z and roughness=1, materialId=0.
            return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 0.0f});
        }
        if (normalEncoding == 1u || normalEncoding == 4u) {
            return vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
        }
        return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f});
    }
}

void VulkanRenderer::clearTemporalGiWriteTargets(uint32_t imageIndex) {
    if (imageIndex >= m_commandBuffers.size()) {
        return;
    }

    struct ClearTarget {
        vk::Image image = VK_NULL_HANDLE;
        vk::ClearColorValue clear{};
    };

    const uint32_t normalEncoding =
        (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->normalEncoding() : 2u;
    const vk::ClearColorValue clearZero(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    const vk::ClearColorValue clearNormalRoughness = nrdNormalRoughnessClearValue(normalEncoding);
    const vk::ClearColorValue clearViewZ(std::array<float, 4>{-1.0f, 0.0f, 0.0f, 0.0f});

    std::vector<ClearTarget> targets;
    targets.reserve(6);
    auto addTarget = [&](vk::Image image, const vk::ClearColorValue &clearValue) {
        if (image != VK_NULL_HANDLE) {
            targets.push_back(ClearTarget{image, clearValue});
        }
    };

    if (!m_nrdPerImage.empty()) {
        const size_t nrdIndex = std::min<size_t>(imageIndex, m_nrdPerImage.size() - 1u);
        const NrdPerImageResources &nrd = m_nrdPerImage[nrdIndex];
        addTarget(*nrd.diffIn.image, clearZero);
        addTarget(*nrd.normalRoughnessIn.image, clearNormalRoughness);
        addTarget(*nrd.motionIn.image, clearZero);
        addTarget(*nrd.viewZIn.image, clearViewZ);
    }

    if (targets.empty()) {
        return;
    }

    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    std::vector<vk::ImageMemoryBarrier> toTransfer;
    std::vector<vk::ImageMemoryBarrier> toShader;
    toTransfer.reserve(targets.size());
    toShader.reserve(targets.size());

    for (const ClearTarget &target : targets) {
        vk::ImageMemoryBarrier beforeClear{};
        beforeClear.oldLayout = vk::ImageLayout::eGeneral;
        beforeClear.newLayout = vk::ImageLayout::eGeneral;
        beforeClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        beforeClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        beforeClear.image = target.image;
        beforeClear.subresourceRange = range;
        beforeClear.srcAccessMask = vk::AccessFlagBits::eShaderRead |
                                    vk::AccessFlagBits::eShaderWrite |
                                    vk::AccessFlagBits::eTransferWrite;
        beforeClear.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        toTransfer.push_back(beforeClear);

        vk::ImageMemoryBarrier afterClear{};
        afterClear.oldLayout = vk::ImageLayout::eGeneral;
        afterClear.newLayout = vk::ImageLayout::eGeneral;
        afterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        afterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        afterClear.image = target.image;
        afterClear.subresourceRange = range;
        afterClear.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        afterClear.dstAccessMask =
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        toShader.push_back(afterClear);
    }

    m_commandBuffers[imageIndex].pipelineBarrier(
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        {},
        {},
        toTransfer
    );

    for (const ClearTarget &target : targets) {
        m_commandBuffers[imageIndex].clearColorImage(
            target.image, vk::ImageLayout::eGeneral, target.clear, range
        );
    }

    m_commandBuffers[imageIndex].pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
        {},
        {},
        {},
        toShader
    );
}
