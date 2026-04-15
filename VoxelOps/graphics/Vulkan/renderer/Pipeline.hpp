#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

enum class PipelineVertexLayout : uint8_t {
    PackedVoxel,
    ModelPosUv
};

class Pipeline {
public:
    struct PushConstants {
        glm::mat4 viewProjection{ 1.0f };
    };

    void create(
        const vk::raii::Device& device,
        const vk::raii::RenderPass& renderPass,
        const vk::raii::DescriptorSetLayout& textureDescriptorSetLayout,
        const vk::raii::DescriptorSetLayout& modelDescriptorSetLayout,
        const vk::raii::DescriptorSetLayout& giDescriptorSetLayout,
        PipelineVertexLayout vertexLayout,
        const char* vertexShaderFile,
        const char* fragmentShaderFile
    );

    void cleanup();

    const vk::raii::PipelineLayout& getLayout() const { return m_pipelineLayout; }
    const vk::raii::Pipeline& getPipeline() const { return m_graphicsPipeline; }

    void bind(const vk::raii::CommandBuffer& commandBuffer) const;
    void pushViewProjection(const vk::raii::CommandBuffer& commandBuffer, const glm::mat4& viewProjection) const;

private:
    vk::raii::PipelineLayout m_pipelineLayout{ nullptr };
    vk::raii::Pipeline m_graphicsPipeline{ nullptr };
};
