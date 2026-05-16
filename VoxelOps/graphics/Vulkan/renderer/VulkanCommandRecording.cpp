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
    float &outUiCpuMs,
    bool recordNrdAndComposite
) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };
    const uint32_t nrdDebugView = frameData.giLighting.nrdDebugView;
    const bool compositeDebugView =
        (nrdDebugView == 10u) || (nrdDebugView == 15u) || (nrdDebugView == 16u) ||
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
    m_lastFrameTimingStats.descriptorBindCount = 0;
    m_lastFrameTimingStats.chunkDescriptorBindCount = 0;
    m_lastFrameTimingStats.modelDescriptorBindCount = 0;
    m_lastFrameTimingStats.drawIndexedIndirectCount = 0;
    m_lastFrameTimingStats.drawIndexedCount = 0;

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
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 1
        );
    }

    const vk::DescriptorSet modelDescriptorSet = *m_modelDescriptorSets[imageIndex];
    const vk::DescriptorSet giDescriptorSet =
        (imageIndex < m_giDescriptorSets.size()) ? *m_giDescriptorSets[imageIndex] : VK_NULL_HANDLE;
    vk::DescriptorSet lastChunkTextureDescriptorSet = VK_NULL_HANDLE;
    bool hasBoundChunkDescriptorSet = false;
    const auto chunkCpuStart = std::chrono::steady_clock::now();

    if (!frameData.indirectCommands.empty() && imageIndex < m_perImageDrawResources.size()) {
        const PerImageDrawResources &resources = m_perImageDrawResources[imageIndex];
        if (resources.indirectCommandBuffer != nullptr) {
            const vk::DeviceSize stride = sizeof(IndexedIndirectCommand);
            const bool supportsMultiDrawIndirect = m_context.isMultiDrawIndirectEnabled();
            const bool supportsIndirectFirstInstance =
                m_context.isDrawIndirectFirstInstanceEnabled();
            const bool useChunkSuperbatch =
                frameData.chunkSuperbatchEnabled &&
                frameData.chunkSuperbatchTexture != nullptr &&
                !frameData.chunkSuperbatchIndices.empty() &&
                resources.chunkSuperbatchVertexBuffer != nullptr &&
                resources.chunkSuperbatchIndexBuffer != nullptr;
            if (useChunkSuperbatch) {
                vk::DescriptorSet textureDescriptorSet = m_fallbackArrayTexture.getDescriptorSet();
                if (frameData.chunkSuperbatchTexture->getDescriptorSet() != VK_NULL_HANDLE) {
                    textureDescriptorSet = frameData.chunkSuperbatchTexture->getDescriptorSet();
                }

                const std::array<vk::DescriptorSet, 3> descriptorSets = {
                    textureDescriptorSet, modelDescriptorSet, giDescriptorSet
                };
                if (!hasBoundChunkDescriptorSet || textureDescriptorSet != lastChunkTextureDescriptorSet) {
                    m_commandBuffers[imageIndex].bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        *m_chunkPipeline.getLayout(),
                        0,
                        descriptorSets,
                        {}
                    );
                    hasBoundChunkDescriptorSet = true;
                    lastChunkTextureDescriptorSet = textureDescriptorSet;
                    ++m_lastFrameTimingStats.descriptorBindCount;
                    ++m_lastFrameTimingStats.chunkDescriptorBindCount;
                }

                const std::array<vk::Buffer, 1> vertexBuffers = {
                    *resources.chunkSuperbatchVertexBuffer
                };
                const std::array<vk::DeviceSize, 1> vertexOffsets = {0};
                m_commandBuffers[imageIndex].bindVertexBuffers(0, vertexBuffers, vertexOffsets);
                m_commandBuffers[imageIndex].bindIndexBuffer(
                    *resources.chunkSuperbatchIndexBuffer, 0, vk::IndexType::eUint16
                );

                if (supportsMultiDrawIndirect && supportsIndirectFirstInstance) {
                    m_commandBuffers[imageIndex].drawIndexedIndirect(
                        *resources.indirectCommandBuffer,
                        0,
                        static_cast<uint32_t>(frameData.indirectCommands.size()),
                        static_cast<uint32_t>(stride)
                    );
                    ++m_lastFrameTimingStats.drawIndexedIndirectCount;
                } else {
                    for (size_t commandIndex = 0; commandIndex < frameData.indirectCommands.size();
                         ++commandIndex) {
                        const IndexedIndirectCommand &command = frameData.indirectCommands[commandIndex];
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
                            ++m_lastFrameTimingStats.drawIndexedCount;
                            continue;
                        }
                        const vk::DeviceSize offset =
                            static_cast<vk::DeviceSize>(commandIndex) * stride;
                        m_commandBuffers[imageIndex].drawIndexedIndirect(
                            *resources.indirectCommandBuffer,
                            offset,
                            1,
                            static_cast<uint32_t>(stride)
                        );
                        ++m_lastFrameTimingStats.drawIndexedIndirectCount;
                    }
                }
            } else {
                for (const RenderIndirectBatch &batch : frameData.indirectBatches) {
                    if (!batch.mesh || batch.commandCount == 0 ||
                        batch.firstCommand >= frameData.indirectCommands.size()) {
                        continue;
                    }

                    const uint32_t maxCommandCount = static_cast<uint32_t>(
                        frameData.indirectCommands.size() - batch.firstCommand
                    );
                    const uint32_t clampedCommandCount =
                        std::min(batch.commandCount, maxCommandCount);
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
                    if (!hasBoundChunkDescriptorSet ||
                        textureDescriptorSet != lastChunkTextureDescriptorSet) {
                        m_commandBuffers[imageIndex].bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            *m_chunkPipeline.getLayout(),
                            0,
                            descriptorSets,
                            {}
                        );
                        hasBoundChunkDescriptorSet = true;
                        lastChunkTextureDescriptorSet = textureDescriptorSet;
                        ++m_lastFrameTimingStats.descriptorBindCount;
                        ++m_lastFrameTimingStats.chunkDescriptorBindCount;
                    }

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
                                ++m_lastFrameTimingStats.drawIndexedCount;
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
                            ++m_lastFrameTimingStats.drawIndexedIndirectCount;
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
                        ++m_lastFrameTimingStats.drawIndexedIndirectCount;
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
                        ++m_lastFrameTimingStats.drawIndexedCount;
                    }
                }
            }
        }
    }
    const auto chunkCpuEnd = std::chrono::steady_clock::now();
    outChunkCpuMs = measureMs(chunkCpuStart, chunkCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 2
        );
    }

    const auto modelCpuStart = std::chrono::steady_clock::now();
    if (!frameData.objects.empty()) {
        m_modelPipeline.bind(m_commandBuffers[imageIndex]);
        m_modelPipeline.pushViewProjection(m_commandBuffers[imageIndex], viewProjection);
        vk::DescriptorSet lastModelTextureDescriptorSet = VK_NULL_HANDLE;
        bool hasBoundModelDescriptorSet = false;

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
                if (!hasBoundModelDescriptorSet || textureDescriptorSet != lastModelTextureDescriptorSet) {
                    m_commandBuffers[imageIndex].bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        *m_modelPipeline.getLayout(),
                        0,
                        descriptorSets,
                        {}
                    );
                    hasBoundModelDescriptorSet = true;
                    lastModelTextureDescriptorSet = textureDescriptorSet;
                    ++m_lastFrameTimingStats.descriptorBindCount;
                    ++m_lastFrameTimingStats.modelDescriptorBindCount;
                }
                m_commandBuffers[imageIndex].drawIndexed(
                    mesh.getIndexCount(), 1, 0, 0, firstInstance
                );
                ++m_lastFrameTimingStats.drawIndexedCount;
            }
        }
    }
    const auto modelCpuEnd = std::chrono::steady_clock::now();
    outModelCpuMs = measureMs(modelCpuStart, modelCpuEnd);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 3
        );
    }

    const auto uiCpuStart = std::chrono::steady_clock::now();
    const auto uiCpuEnd = std::chrono::steady_clock::now();
    outUiCpuMs = measureMs(uiCpuStart, uiCpuEnd);
    m_commandBuffers[imageIndex].endRenderPass();
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 4
        );
    }
    if (recordNrdAndComposite) {
        barrierNrdSignalsForCompute(*m_commandBuffers[imageIndex], imageIndex);
#if VOXELOPS_NRD_HEADERS
        dispatchNrdPass(*m_commandBuffers[imageIndex], imageIndex, frameData);
#endif
        if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
            m_commandBuffers[imageIndex].writeTimestamp(
                vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 5
            );
        }
        if (useNrdComposite) {
            barrierNrdSignalsForComposite(*m_commandBuffers[imageIndex], imageIndex);
        }
        recordNrdCompositePass(*m_commandBuffers[imageIndex], imageIndex, frameData, useNrdComposite);
        if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
            m_commandBuffers[imageIndex].writeTimestamp(
                vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 6
            );
        }
    }
    m_commandBuffers[imageIndex].end();
}

void VulkanRenderer::recordNrdCompositePass(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const FrameRenderData &frameData,
    bool applyNrdComposite
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
    commandBuffer.pipelineBarrier(
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

    commandBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

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
    commandBuffer.setScissor(0, scissor);

    const bool usePostProcess = applyNrdComposite && (frameData.giLighting.nrdDebugView == 0u) &&
                                m_postProcessPipeline != nullptr &&
                                m_postProcessPipelineLayout != nullptr;
    if (usePostProcess) {
        commandBuffer.bindPipeline(
            vk::PipelineBindPoint::eGraphics, *m_postProcessPipeline
        );
        const vk::DescriptorSet giSet = *m_giDescriptorSets[imageIndex];
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, *m_postProcessPipelineLayout, 0, giSet, {}
        );
        commandBuffer.draw(3, 1, 0, 0);
    } else if (applyNrdComposite && m_nrdCompositePipeline != nullptr &&
               m_nrdCompositePipelineLayout != nullptr) {
        commandBuffer.bindPipeline(
            vk::PipelineBindPoint::eGraphics, *m_nrdCompositePipeline
        );
        const vk::DescriptorSet giSet = *m_giDescriptorSets[imageIndex];
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, *m_nrdCompositePipelineLayout, 0, giSet, {}
        );
        commandBuffer.draw(3, 1, 0, 0);
    }

#if VOXELOPS_IMGUI_VULKAN_BACKEND_AVAILABLE
    ImDrawData *drawData = frameData.uiDrawData;
    if (drawData != nullptr && drawData->CmdListsCount > 0 &&
        ImGui::GetCurrentContext() != nullptr) {
            ImGuiIO &io = ImGui::GetIO();
        if (io.BackendRendererUserData != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        }
    }
#endif

    commandBuffer.endRenderPass();
}

#if VOXELOPS_NRD_HEADERS
void VulkanRenderer::recordNrdComputeCommandBuffer(
    uint32_t imageIndex, const FrameRenderData &frameData
) {
    if (imageIndex >= m_nrdComputeCommandBuffers.size()) {
        return;
    }

    vk::CommandBufferBeginInfo beginInfo{};
    m_nrdComputeCommandBuffers[imageIndex].begin(beginInfo);
    barrierNrdSignalsForCompute(*m_nrdComputeCommandBuffers[imageIndex], imageIndex);
    dispatchNrdPass(*m_nrdComputeCommandBuffers[imageIndex], imageIndex, frameData);
    m_nrdComputeCommandBuffers[imageIndex].end();
}
#endif

void VulkanRenderer::recordCompositeCommandBuffer(
    uint32_t imageIndex, const FrameRenderData &frameData, bool applyNrdComposite
) {
    if (imageIndex >= m_compositeCommandBuffers.size()) {
        return;
    }

    vk::CommandBufferBeginInfo beginInfo{};
    m_compositeCommandBuffers[imageIndex].begin(beginInfo);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_compositeCommandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eTopOfPipe, *m_timestampQueryPools[imageIndex], 5
        );
    }
    if (applyNrdComposite) {
        barrierNrdSignalsForComposite(*m_compositeCommandBuffers[imageIndex], imageIndex);
    }
    recordNrdCompositePass(
        *m_compositeCommandBuffers[imageIndex], imageIndex, frameData, applyNrdComposite
    );
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_compositeCommandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 6
        );
    }
    m_compositeCommandBuffers[imageIndex].end();
}


