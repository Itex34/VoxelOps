#include "graphics/Vulkan/renderer/FrameSync.hpp"

void FrameSync::init(const vk::raii::Device &device, uint32_t maxFramesInFlight) {
    m_imageAvailableSemaphores.clear();
    m_inFlightFences.clear();

    m_imageAvailableSemaphores.reserve(maxFramesInFlight);
    m_inFlightFences.reserve(maxFramesInFlight);

    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};

    for (uint32_t i = 0; i < maxFramesInFlight; ++i) {
        m_imageAvailableSemaphores.emplace_back(device, semaphoreInfo);
        m_inFlightFences.emplace_back(device, fenceInfo);
    }

    m_currentFrame = 0;
}

void FrameSync::recreateSwapchainSync(const vk::raii::Device &device, size_t swapchainImageCount) {
    m_renderFinishedSemaphores.clear();
    m_imagesInFlight.clear();

    m_renderFinishedSemaphores.reserve(swapchainImageCount);
    m_imagesInFlight.assign(swapchainImageCount, vk::Fence{});

    vk::SemaphoreCreateInfo semaphoreInfo{};
    for (size_t i = 0; i < swapchainImageCount; ++i) {
        m_renderFinishedSemaphores.emplace_back(device, semaphoreInfo);
    }
}

void FrameSync::cleanup() {
    m_imagesInFlight.clear();
    m_renderFinishedSemaphores.clear();
    m_imageAvailableSemaphores.clear();
    m_inFlightFences.clear();
    m_currentFrame = 0;
}
