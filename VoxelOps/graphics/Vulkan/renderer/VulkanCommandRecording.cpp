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
#include <array>
#include <chrono>

namespace {
    float MeasureMs(
        const std::chrono::steady_clock::time_point &start,
        const std::chrono::steady_clock::time_point &end
    ) {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    }

    vk::ClearColorValue NrdNormalRoughnessClearValue(uint32_t normalEncoding) {
        if (normalEncoding == 2u) {
            return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 0.0f});
        }
        if (normalEncoding == 1u || normalEncoding == 4u) {
            return vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
        }
        return vk::ClearColorValue(std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f});
    }

    void SetFullViewportAndScissor(vk::CommandBuffer commandBuffer, vk::Extent2D extent) {
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
    }

    void SetFullViewportAndScissor(const vk::raii::CommandBuffer &commandBuffer, vk::Extent2D extent) {
        SetFullViewportAndScissor(*commandBuffer, extent);
    }
}

void VulkanRenderer::recordCommandBuffer(
    uint32_t imageIndex,
    const glm::mat4 &viewProjection,
    const FrameRenderData &frameData,
    float &outChunkCpuMs,
    float &outModelCpuMs,
    float &outUiCpuMs,
    bool recordNrdAndComposite
) {
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
    beginMainSceneRenderPass(m_commandBuffers[imageIndex], imageIndex);
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 1
        );
    }

    const vk::DescriptorSet modelDescriptorSet = *m_modelDescriptorSets[imageIndex];
    const vk::DescriptorSet giDescriptorSet =
        (imageIndex < m_giDescriptorSets.size()) ? *m_giDescriptorSets[imageIndex] : VK_NULL_HANDLE;
    outChunkCpuMs = recordChunkPass(
        m_commandBuffers[imageIndex],
        imageIndex,
        viewProjection,
        frameData,
        modelDescriptorSet,
        giDescriptorSet
    );
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 2
        );
    }

    outModelCpuMs = recordModelPass(
        m_commandBuffers[imageIndex],
        imageIndex,
        viewProjection,
        frameData,
        modelDescriptorSet,
        giDescriptorSet
    );
    if (m_timestampQueriesEnabled && imageIndex < m_timestampQueryPools.size()) {
        m_commandBuffers[imageIndex].writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe, *m_timestampQueryPools[imageIndex], 3
        );
    }

    outUiCpuMs = recordUiPass(m_commandBuffers[imageIndex], imageIndex, frameData);
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

void VulkanRenderer::beginMainSceneRenderPass(
    const vk::raii::CommandBuffer &commandBuffer, uint32_t imageIndex
) {
    const uint32_t normalEncoding =
        (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->normalEncoding() : 2u;
    std::array<vk::ClearValue, 8> clearValues{};
    clearValues[0].color = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.07f, 0.10f, 1.0f});
    clearValues[1].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[2].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[3].color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    clearValues[4].color = NrdNormalRoughnessClearValue(normalEncoding);
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

    commandBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);
    SetFullViewportAndScissor(commandBuffer, m_context.getSwapchainExtent());
}

float VulkanRenderer::recordChunkPass(
    const vk::raii::CommandBuffer &commandBuffer,
    uint32_t imageIndex,
    const glm::mat4 &viewProjection,
    const FrameRenderData &frameData,
    vk::DescriptorSet modelDescriptorSet,
    vk::DescriptorSet giDescriptorSet
) {
    const auto chunkCpuStart = std::chrono::steady_clock::now();
    m_chunkPipeline.bind(commandBuffer);
    m_chunkPipeline.pushViewProjection(commandBuffer, viewProjection);

    vk::DescriptorSet lastChunkTextureDescriptorSet = VK_NULL_HANDLE;
    bool hasBoundChunkDescriptorSet = false;

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
                    commandBuffer.bindDescriptorSets(
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
                commandBuffer.bindVertexBuffers(0, vertexBuffers, vertexOffsets);
                commandBuffer.bindIndexBuffer(
                    *resources.chunkSuperbatchIndexBuffer, 0, vk::IndexType::eUint16
                );

                if (supportsMultiDrawIndirect && supportsIndirectFirstInstance) {
                    commandBuffer.drawIndexedIndirect(
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
                            commandBuffer.drawIndexed(
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
                        commandBuffer.drawIndexedIndirect(
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

                    batch.mesh->bind(commandBuffer);

                    vk::DescriptorSet textureDescriptorSet = m_fallbackArrayTexture.getDescriptorSet();
                    if (batch.texture && batch.texture->getDescriptorSet() != VK_NULL_HANDLE) {
                        textureDescriptorSet = batch.texture->getDescriptorSet();
                    }

                    const std::array<vk::DescriptorSet, 3> descriptorSets = {
                        textureDescriptorSet, modelDescriptorSet, giDescriptorSet
                    };
                    if (!hasBoundChunkDescriptorSet ||
                        textureDescriptorSet != lastChunkTextureDescriptorSet) {
                        commandBuffer.bindDescriptorSets(
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
                                commandBuffer.drawIndexed(
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
                            commandBuffer.drawIndexedIndirect(
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
                        commandBuffer.drawIndexedIndirect(
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
                        commandBuffer.drawIndexed(
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

    return MeasureMs(chunkCpuStart, std::chrono::steady_clock::now());
}

float VulkanRenderer::recordModelPass(
    const vk::raii::CommandBuffer &commandBuffer,
    uint32_t,
    const glm::mat4 &viewProjection,
    const FrameRenderData &frameData,
    vk::DescriptorSet modelDescriptorSet,
    vk::DescriptorSet giDescriptorSet
) {
    const auto modelCpuStart = std::chrono::steady_clock::now();
    if (!frameData.objects.empty()) {
        m_modelPipeline.bind(commandBuffer);
        m_modelPipeline.pushViewProjection(commandBuffer, viewProjection);
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

                mesh.bind(commandBuffer);

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
                if (!hasBoundModelDescriptorSet ||
                    textureDescriptorSet != lastModelTextureDescriptorSet) {
                    commandBuffer.bindDescriptorSets(
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
                commandBuffer.drawIndexed(mesh.getIndexCount(), 1, 0, 0, firstInstance);
                ++m_lastFrameTimingStats.drawIndexedCount;
            }
        }
    }
    return MeasureMs(modelCpuStart, std::chrono::steady_clock::now());
}

float VulkanRenderer::recordUiPass(
    const vk::raii::CommandBuffer &,
    uint32_t,
    const FrameRenderData &
) {
    const auto uiCpuStart = std::chrono::steady_clock::now();
    return MeasureMs(uiCpuStart, std::chrono::steady_clock::now());
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

    SetFullViewportAndScissor(commandBuffer, extent);
    recordCompositeFullscreen(commandBuffer, imageIndex, frameData, applyNrdComposite);
    recordCompositeOverlays(commandBuffer, imageIndex, frameData, extent);

    commandBuffer.endRenderPass();
}

void VulkanRenderer::recordCompositeFullscreen(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const FrameRenderData &frameData,
    bool applyNrdComposite
) {
    const bool usePostProcess = applyNrdComposite && (frameData.giLighting.nrdDebugView == 0u) &&
                                m_postProcessPipeline != nullptr &&
                                m_postProcessPipelineLayout != nullptr;
    if (usePostProcess) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_postProcessPipeline);
        const vk::DescriptorSet giSet = *m_giDescriptorSets[imageIndex];
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, *m_postProcessPipelineLayout, 0, giSet, {}
        );
        commandBuffer.draw(3, 1, 0, 0);
        return;
    }

    if (applyNrdComposite && m_nrdCompositePipeline != nullptr &&
        m_nrdCompositePipelineLayout != nullptr) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_nrdCompositePipeline);
        const vk::DescriptorSet giSet = *m_giDescriptorSets[imageIndex];
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, *m_nrdCompositePipelineLayout, 0, giSet, {}
        );
        commandBuffer.draw(3, 1, 0, 0);
    }
}

void VulkanRenderer::recordCompositeOverlays(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const FrameRenderData &frameData,
    vk::Extent2D extent
) {
    if (frameData.nativeUiDrawData != nullptr && m_nativeUiRenderer.isInitialized()) {
        m_nativeUiRenderer.render(commandBuffer, imageIndex, *frameData.nativeUiDrawData, extent);
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


