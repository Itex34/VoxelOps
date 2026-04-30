#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/graphics/Model.hpp"

#include <imgui.h>
#if __has_include(<imgui_impl_vulkan.h>)
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 1
#include <imgui_impl_vulkan.h>
#else
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 0
#endif

#include <algorithm>
#include <chrono>

namespace {
constexpr uint32_t kRestirHistorySlot = 0u;
} // namespace

void VulkanRenderer::recordCommandBuffer(uint32_t imageIndex, const glm::mat4 &viewProjection,
                                         const FrameRenderData &frameData, float &outChunkCpuMs,
                                         float &outModelCpuMs, float &outUiCpuMs) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };
    outChunkCpuMs = 0.0f;
    outModelCpuMs = 0.0f;
    outUiCpuMs = 0.0f;

    vk::CommandBufferBeginInfo beginInfo{};
    m_commandBuffers[imageIndex].begin(beginInfo);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].resetQueryPool(*m_timestampQueryPools[imageIndex], 0,
                                                    TIMESTAMP_QUERY_COUNT);
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 0);
    }
    clearTemporalGiWriteTargets(imageIndex);
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

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, reservoirBarriers);
    }

    if (historyIndex < m_restirValidationPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirValidationPerImage[historyIndex];

        const vk::ImageSubresourceRange validationRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                                        1);
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

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, validationBarriers);
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

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, metaBarriers);
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

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, giBarriers);
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

        m_commandBuffers[imageIndex].pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                                                     vk::PipelineStageFlagBits::eFragmentShader, {},
                                                     {}, {}, giMetaBarriers);
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
            {}, {}, {}, spatialBarriers);
    }

    if (historyIndex < m_restirGiSpatialMetaPerImage.size() &&
        historyIndex < m_restirDiWriteParityPerImage.size()) {
        const uint32_t writeParity = m_restirDiWriteParityPerImage[historyIndex] & 1u;
        const uint32_t readParity = writeParity ^ 1u;
        const auto &perImage = m_restirGiSpatialMetaPerImage[historyIndex];

        const vk::ImageSubresourceRange spatialMetaRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                                         1);
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
            {}, {}, {}, spatialMetaBarriers);
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
            vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, nrdHistoryBarrier);
    }

    std::array<vk::ClearValue, 2> clearValues{};
    clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f});
    clearValues[1].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

    vk::RenderPassBeginInfo renderPassInfo{};
    renderPassInfo.renderPass = *m_renderPass.get();
    renderPassInfo.framebuffer = *m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = vk::Offset2D{0, 0};
    renderPassInfo.renderArea.extent = m_context.getSwapchainExtent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    m_commandBuffers[imageIndex].beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

    m_chunkPipeline.bind(m_commandBuffers[imageIndex]);
    m_chunkPipeline.pushViewProjection(m_commandBuffers[imageIndex], viewProjection);

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    m_commandBuffers[imageIndex].setViewport(0, viewport);

    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{0, 0};
    scissor.extent = extent;
    m_commandBuffers[imageIndex].setScissor(0, scissor);

    const vk::DescriptorSet modelDescriptorSet = *m_modelDescriptorSets[imageIndex];
    const vk::DescriptorSet giDescriptorSet =
        (imageIndex < m_giDescriptorSets.size()) ? *m_giDescriptorSets[imageIndex] : VK_NULL_HANDLE;
    const auto chunkCpuStart = std::chrono::steady_clock::now();

    if (!frameData.indirectCommands.empty() && imageIndex < m_perImageDrawResources.size()) {
        const PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
        if (resources.indirectCommandBuffer != nullptr) {
            const vk::DeviceSize stride = sizeof(IndexedIndirectCommand);
            const bool supportsMultiDrawIndirect = m_context.isMultiDrawIndirectEnabled();
            const bool supportsIndirectFirstInstance =
                m_context.isDrawIndirectFirstInstanceEnabled();
            for (const RenderIndirectBatch &batch : frameData.indirectBatches) {
                if (!batch.mesh || batch.commandCount == 0 ||
                    batch.firstCommand >= frameData.indirectCommands.size()) {
                    continue;
                }

                const uint32_t maxCommandCount =
                    static_cast<uint32_t>(frameData.indirectCommands.size() - batch.firstCommand);
                const uint32_t clampedCommandCount = std::min(batch.commandCount, maxCommandCount);
                if (clampedCommandCount == 0) {
                    continue;
                }

                batch.mesh->bind(m_commandBuffers[imageIndex]);

                vk::DescriptorSet textureDescriptorSet = m_fallbackArrayTexture.getDescriptorSet();
                if (batch.texture && batch.texture->getDescriptorSet() != VK_NULL_HANDLE) {
                    textureDescriptorSet = batch.texture->getDescriptorSet();
                }

                const std::array<vk::DescriptorSet, 3> descriptorSets = {
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet};
                m_commandBuffers[imageIndex].bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                                                *m_chunkPipeline.getLayout(), 0,
                                                                descriptorSets, {});

                if (!supportsMultiDrawIndirect && clampedCommandCount > 1) {
                    for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                        const IndexedIndirectCommand &command =
                            frameData.indirectCommands[batch.firstCommand + i];
                        if (command.indexCount == 0 || command.instanceCount == 0) {
                            continue;
                        }

                        if (!supportsIndirectFirstInstance && command.firstInstance != 0) {
                            m_commandBuffers[imageIndex].drawIndexed(
                                command.indexCount, command.instanceCount, command.firstIndex,
                                command.vertexOffset, command.firstInstance);
                            continue;
                        }

                        const vk::DeviceSize offset =
                            static_cast<vk::DeviceSize>(batch.firstCommand + i) * stride;
                        m_commandBuffers[imageIndex].drawIndexedIndirect(
                            *resources.indirectCommandBuffer, offset, 1,
                            static_cast<uint32_t>(stride));
                    }
                    continue;
                }

                bool requiresNonZeroFirstInstance = false;
                if (!supportsIndirectFirstInstance) {
                    for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                        const IndexedIndirectCommand &command =
                            frameData.indirectCommands[batch.firstCommand + i];
                        if (command.firstInstance != 0) {
                            requiresNonZeroFirstInstance = true;
                            break;
                        }
                    }
                }

                if (!requiresNonZeroFirstInstance) {
                    const vk::DeviceSize offset =
                        static_cast<vk::DeviceSize>(batch.firstCommand) * stride;
                    m_commandBuffers[imageIndex].drawIndexedIndirect(
                        *resources.indirectCommandBuffer, offset, clampedCommandCount,
                        static_cast<uint32_t>(stride));
                    continue;
                }

                for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                    const IndexedIndirectCommand &command =
                        frameData.indirectCommands[batch.firstCommand + i];
                    if (command.indexCount == 0 || command.instanceCount == 0) {
                        continue;
                    }
                    m_commandBuffers[imageIndex].drawIndexed(
                        command.indexCount, command.instanceCount, command.firstIndex,
                        command.vertexOffset, command.firstInstance);
                }
            }
        }
    }
    const auto chunkCpuEnd = std::chrono::steady_clock::now();
    outChunkCpuMs = measureMs(chunkCpuStart, chunkCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 1);
    }

    const auto modelCpuStart = std::chrono::steady_clock::now();
    if (!frameData.objects.empty()) {
        m_modelPipeline.bind(m_commandBuffers[imageIndex]);
        m_modelPipeline.pushViewProjection(m_commandBuffers[imageIndex], viewProjection);

        const uint32_t modelBaseInstance = static_cast<uint32_t>(frameData.modelMatrices.size());
        uint32_t objectIndex = 0;
        for (const RenderObject &object : frameData.objects) {
            const uint32_t firstInstance = modelBaseInstance + objectIndex;
            ++objectIndex;

            if (object.model == nullptr) {
                continue;
            }

            const std::vector<VkMesh> &meshes = object.model->getMeshes();
            if (meshes.empty()) {
                continue;
            }

            for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
                const VkMesh &mesh = meshes[meshIndex];
                if (mesh.getIndexCount() == 0) {
                    continue;
                }

                mesh.bind(m_commandBuffers[imageIndex]);

                vk::DescriptorSet textureDescriptorSet = m_fallback2DTexture.getDescriptorSet();
                if (object.meshTextures != nullptr && meshIndex < object.meshTextures->size()) {
                    const VkTexture *texture = (*object.meshTextures)[meshIndex];
                    if (texture != nullptr && texture->getDescriptorSet() != VK_NULL_HANDLE) {
                        textureDescriptorSet = texture->getDescriptorSet();
                    }
                }

                const std::array<vk::DescriptorSet, 3> descriptorSets = {
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet};
                m_commandBuffers[imageIndex].bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                                                *m_modelPipeline.getLayout(), 0,
                                                                descriptorSets, {});
                m_commandBuffers[imageIndex].drawIndexed(mesh.getIndexCount(), 1, 0, 0,
                                                         firstInstance);
            }
        }
    }
    const auto modelCpuEnd = std::chrono::steady_clock::now();
    outModelCpuMs = measureMs(modelCpuStart, modelCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 2);
    }

    const auto uiCpuStart = std::chrono::steady_clock::now();
#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    ImDrawData *drawData = frameData.uiDrawData;
    if (drawData != nullptr && drawData->CmdListsCount > 0 && ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO &io = ImGui::GetIO();
        if (io.BackendRendererUserData != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(drawData, *m_commandBuffers[imageIndex]);
        }
    }
#endif
    const auto uiCpuEnd = std::chrono::steady_clock::now();
    outUiCpuMs = measureMs(uiCpuStart, uiCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 3);
    }

    m_commandBuffers[imageIndex].endRenderPass();
    barrierNrdSignalsForCompute(imageIndex);
#if VOXELOPS_NRD_HEADERS
    dispatchNrdPass(imageIndex, frameData);
#endif
    dispatchRestirGiSpatialPass(imageIndex, frameData);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                                    *m_timestampQueryPools[imageIndex], 4);
    }
    m_commandBuffers[imageIndex].end();
}
