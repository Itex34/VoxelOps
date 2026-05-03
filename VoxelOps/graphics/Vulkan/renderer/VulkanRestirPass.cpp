#include "VulkanRenderer.hpp"
#include "RestirConfig.hpp"

#include <array>

using namespace Vulkan::Restir;

void VulkanRenderer::recordRestirPrePassBarriers(uint32_t imageIndex) {
    const uint32_t historyIndex = kRestirHistorySlot;

    if (historyIndex < m_restirDiPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirDiPerImage[historyIndex];

        const vk::ImageSubresourceRange reservoirRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> reservoirBarriers{};
        reservoirBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[0].image = *perImage[readParity].image;
        reservoirBarriers[0].subresourceRange = reservoirRange;
        reservoirBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        reservoirBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        reservoirBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        reservoirBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        reservoirBarriers[1].image = *perImage[writeParity].image;
        reservoirBarriers[1].subresourceRange = reservoirRange;
        reservoirBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        reservoirBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            {},
            {},
            reservoirBarriers
        );
    }

    if (historyIndex < m_restirValidationPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirValidationPerImage[historyIndex];

        const vk::ImageSubresourceRange validationRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
        );
        std::array<vk::ImageMemoryBarrier, 2> validationBarriers{};
        validationBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        validationBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        validationBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[0].image = *perImage[readParity].image;
        validationBarriers[0].subresourceRange = validationRange;
        validationBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        validationBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        validationBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        validationBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        validationBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        validationBarriers[1].image = *perImage[writeParity].image;
        validationBarriers[1].subresourceRange = validationRange;
        validationBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        validationBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            {},
            {},
            validationBarriers
        );
    }

    if (historyIndex < m_restirMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange metaRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> metaBarriers{};
        metaBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        metaBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        metaBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[0].image = *perImage[readParity].image;
        metaBarriers[0].subresourceRange = metaRange;
        metaBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        metaBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        metaBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        metaBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        metaBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        metaBarriers[1].image = *perImage[writeParity].image;
        metaBarriers[1].subresourceRange = metaRange;
        metaBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        metaBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            {},
            {},
            metaBarriers
        );
    }

    if (historyIndex < m_restirGiPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiPerImage[historyIndex];

        const vk::ImageSubresourceRange giRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> giBarriers{};
        giBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        giBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        giBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[0].image = *perImage[readParity].image;
        giBarriers[0].subresourceRange = giRange;
        giBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        giBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        giBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        giBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giBarriers[1].image = *perImage[writeParity].image;
        giBarriers[1].subresourceRange = giRange;
        giBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            {},
            {},
            giBarriers
        );
    }

    if (historyIndex < m_restirGiMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange giMetaRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> giMetaBarriers{};
        giMetaBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[0].image = *perImage[readParity].image;
        giMetaBarriers[0].subresourceRange = giMetaRange;
        giMetaBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giMetaBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        giMetaBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        giMetaBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        giMetaBarriers[1].image = *perImage[writeParity].image;
        giMetaBarriers[1].subresourceRange = giMetaRange;
        giMetaBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        giMetaBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            {},
            {},
            giMetaBarriers
        );
    }

    if (historyIndex < m_restirGiSpatialPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiSpatialPerImage[historyIndex];

        const vk::ImageSubresourceRange spatialRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::array<vk::ImageMemoryBarrier, 2> spatialBarriers{};
        spatialBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[0].image = *perImage[readParity].image;
        spatialBarriers[0].subresourceRange = spatialRange;
        spatialBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        spatialBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        spatialBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialBarriers[1].image = *perImage[writeParity].image;
        spatialBarriers[1].subresourceRange = spatialRange;
        spatialBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            {},
            {},
            {},
            spatialBarriers
        );
    }

    if (historyIndex < m_restirGiSpatialMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiSpatialMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange spatialMetaRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
        );
        std::array<vk::ImageMemoryBarrier, 2> spatialMetaBarriers{};
        spatialMetaBarriers[0].oldLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[0].newLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[0].image = *perImage[readParity].image;
        spatialMetaBarriers[0].subresourceRange = spatialMetaRange;
        spatialMetaBarriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialMetaBarriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;

        spatialMetaBarriers[1].oldLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[1].newLayout = vk::ImageLayout::eGeneral;
        spatialMetaBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        spatialMetaBarriers[1].image = *perImage[writeParity].image;
        spatialMetaBarriers[1].subresourceRange = spatialMetaRange;
        spatialMetaBarriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        spatialMetaBarriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;

        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            {},
            {},
            {},
            spatialMetaBarriers
        );
    }

    if (!m_nrdPerImage.empty()) {
        const vk::ImageSubresourceRange nrdRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier nrdHistoryBarrier{};
        nrdHistoryBarrier.oldLayout = vk::ImageLayout::eGeneral;
        nrdHistoryBarrier.newLayout = vk::ImageLayout::eGeneral;
        nrdHistoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        nrdHistoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        nrdHistoryBarrier.image = *m_nrdPerImage[0].diffOut.image;
        nrdHistoryBarrier.subresourceRange = nrdRange;
        nrdHistoryBarrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        nrdHistoryBarrier.dstAccessMask =
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        m_commandBuffers[imageIndex].pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            {},
            {},
            nrdHistoryBarrier
        );
    }
}
