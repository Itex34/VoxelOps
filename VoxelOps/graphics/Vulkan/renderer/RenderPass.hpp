#pragma once

#include <vulkan/vulkan_raii.hpp>

class RenderPass {
public:
    void create(const vk::raii::Device& device, vk::Format colorFormat, vk::Format depthFormat);
    void cleanup();

    const vk::raii::RenderPass& get() const { return m_renderPass; }

private:
    vk::raii::RenderPass m_renderPass{ nullptr };
};
