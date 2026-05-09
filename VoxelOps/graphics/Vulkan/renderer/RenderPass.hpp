#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <vector>

class RenderPass {
public:
    void create(
        const vk::raii::Device &device,
        vk::Format colorFormat,
        vk::Format depthFormat,
        bool withDepth,
        vk::AttachmentLoadOp colorLoadOp,
        vk::ImageLayout colorInitialLayout,
        vk::ImageLayout colorFinalLayout,
        const std::vector<vk::Format> &extraColorFormats = {}
    );
    void cleanup();

    const vk::raii::RenderPass &get() const {
        return m_renderPass;
    }

private:
    vk::raii::RenderPass m_renderPass{nullptr};
};
