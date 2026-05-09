#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/graphics/Model.hpp"
#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"
#include <imgui.h>
#if __has_include(<imgui_impl_vulkan.h>)
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 1
#include <imgui_impl_vulkan.h>
#else
#define VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE 0
#endif

#include <algorithm>
#include <chrono>

void VulkanRenderer::recordCommandBuffer(
    uint32_t imageIndex,
    const glm::mat4 &viewProjection,
    const FrameRenderData &frameData,
    float &outChunkCpuMs,
    float &outModelCpuMs,
    float &outUiCpuMs
) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };
    const uint32_t nrdDebugView = frameData.giLighting.nrdDebugView;
    const bool compositeDebugView =
        ((nrdDebugView >= 10u) && (nrdDebugView <= 18u)) ||
        (nrdDebugView == 24u) || (nrdDebugView == 25u) ||
        (nrdDebugView == 26u) || (nrdDebugView == 27u) ||
        ((nrdDebugView >= 30u) && (nrdDebugView <= 34u));
    const bool useNrdComposite = frameData.giLighting.pathTracingEnabled &&
                                 (nrdDebugView == 0u || compositeDebugView) &&
                                 m_nrdBootstrap != nullptr && m_nrdBootstrap->isActive() &&
                                 m_nrdCompositePipeline != nullptr &&
                                 imageIndex < m_compositeFramebuffers.size() &&
                                 imageIndex < m_giDescriptorSets.size();
    outChunkCpuMs = 0.0f;
    outModelCpuMs = 0.0f;
    outUiCpuMs = 0.0f;

    vk::CommandBufferBeginInfo beginInfo{};
    m_commandBuffers[imageIndex].begin(beginInfo);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].resetQueryPool(
            *m_timestampQueryPools[imageIndex], 0, TIMESTAMP_QUERY_COUNT
        );
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eTopOfPipe, *m_timestampQueryPools[imageIndex], 0
        );
    }
    clearTemporalGiWriteTargets(imageIndex);

    auto nrdNormalRoughnessClear = [&](uint32_t normalEncoding) -> vk::ClearColorValue {
        if (normalEncoding == 2u) {
            return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 0.0f});
        }
        if (normalEncoding == 1u || normalEncoding == 4u) {
            return vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
        }
        return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f});
    };
    const uint32_t normalEncoding =
        (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->normalEncoding() : 2u;
    std::array<vk::ClearValue, 8> clearValues{};
    clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f});
    clearValues[1].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[2].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[3].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[4].color = nrdNormalRoughnessClear(normalEncoding);
    clearValues[5].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[6].color = vk::ClearColorValue(std::array<float, 4>{-1.0f, 0.0f, 0.0f, 0.0f});
    clearValues[7].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

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
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet
                };
                m_commandBuffers[imageIndex].bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    *m_chunkPipeline.getLayout(),
                    0,
                    descriptorSets,
                    {}
                );

                if (!supportsMultiDrawIndirect && clampedCommandCount > 1) {
                    for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                        const IndexedIndirectCommand &command =
                            frameData.indirectCommands[batch.firstCommand + i];
                        if (command.indexCount == 0 || command.instanceCount == 0) {
                            continue;
                        }

                        if (!supportsIndirectFirstInstance && command.firstInstance != 0) {
                            m_commandBuffers[imageIndex].drawIndexed(
                                command.indexCount,
                                command.instanceCount,
                                command.firstIndex,
                                command.vertexOffset,
                                command.firstInstance
                            );
                            continue;
                        }

                        const vk::DeviceSize offset =
                            static_cast<vk::DeviceSize>(batch.firstCommand + i) * stride;
                        m_commandBuffers[imageIndex].drawIndexedIndirect(
                            *resources.indirectCommandBuffer,
                            offset,
                            1,
                            static_cast<uint32_t>(stride)
                        );
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
                        *resources.indirectCommandBuffer,
                        offset,
                        clampedCommandCount,
                        static_cast<uint32_t>(stride)
                    );
                    continue;
                }

                for (uint32_t i = 0; i < clampedCommandCount; ++i) {
                    const IndexedIndirectCommand &command =
                        frameData.indirectCommands[batch.firstCommand + i];
                    if (command.indexCount == 0 || command.instanceCount == 0) {
                        continue;
                    }
                    m_commandBuffers[imageIndex].drawIndexed(
                        command.indexCount,
                        command.instanceCount,
                        command.firstIndex,
                        command.vertexOffset,
                        command.firstInstance
                    );
                }
            }
        }
    }
    const auto chunkCpuEnd = std::chrono::steady_clock::now();
    outChunkCpuMs = measureMs(chunkCpuStart, chunkCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 1
        );
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
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet
                };
                m_commandBuffers[imageIndex].bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    *m_modelPipeline.getLayout(),
                    0,
                    descriptorSets,
                    {}
                );
                m_commandBuffers[imageIndex].drawIndexed(
                    mesh.getIndexCount(), 1, 0, 0, firstInstance
                );
            }
        }
    }
    const auto modelCpuEnd = std::chrono::steady_clock::now();
    outModelCpuMs = measureMs(modelCpuStart, modelCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 2
        );
    }

    const auto uiCpuStart = std::chrono::steady_clock::now();
    const auto uiCpuEnd = std::chrono::steady_clock::now();
    outUiCpuMs = measureMs(uiCpuStart, uiCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 3
        );
    }

    m_commandBuffers[imageIndex].endRenderPass();
    barrierNrdSignalsForCompute(imageIndex);
#if VOXELOPS_NRD_HEADERS
    dispatchNrdPass(imageIndex, frameData);
#endif
    if (useNrdComposite) {
        barrierNrdSignalsForComposite(imageIndex);
    }
    recordNrdCompositePass(imageIndex, frameData, useNrdComposite);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 4
        );
    }
    m_commandBuffers[imageIndex].end();
}

void VulkanRenderer::recordNrdCompositePass(
    uint32_t imageIndex, const FrameRenderData &frameData, bool applyNrdComposite
) {
    if (imageIndex >= m_compositeFramebuffers.size() || imageIndex >= m_giDescriptorSets.size() ||
        m_compositeRenderPass.get() == nullptr) {
        return;
    }

    const vk::Extent2D extent = m_context.getSwapchainExtent();
    vk::MemoryBarrier colorBarrier{};
    colorBarrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    colorBarrier.dstAccessMask =
        vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
    m_commandBuffers[imageIndex].pipelineBarrier(
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        {},
        colorBarrier,
        {},
        {}
    );

    vk::RenderPassBeginInfo renderPassInfo{};
    renderPassInfo.renderPass = *m_compositeRenderPass.get();
    renderPassInfo.framebuffer = *m_compositeFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = vk::Offset2D{0, 0};
    renderPassInfo.renderArea.extent = extent;

    m_commandBuffers[imageIndex].beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

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

    if (applyNrdComposite && m_nrdCompositePipeline != nullptr &&
        m_nrdCompositePipelineLayout != nullptr) {
        m_commandBuffers[imageIndex].bindPipeline(
            vk::PipelineBindPoint::eGraphics, *m_nrdCompositePipeline
        );
        const vk::DescriptorSet giSet = *m_giDescriptorSets[imageIndex];
        m_commandBuffers[imageIndex].bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, *m_nrdCompositePipelineLayout, 0, giSet, {}
        );
        m_commandBuffers[imageIndex].draw(3, 1, 0, 0);
    }

#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    ImDrawData *drawData = frameData.uiDrawData;
    if (drawData != nullptr && drawData->CmdListsCount > 0 &&
        ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO &io = ImGui::GetIO();
        if (io.BackendRendererUserData != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(drawData, *m_commandBuffers[imageIndex]);
        }
    }
#endif

    m_commandBuffers[imageIndex].endRenderPass();
}
