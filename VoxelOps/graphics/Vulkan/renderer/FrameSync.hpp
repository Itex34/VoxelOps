#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <vector>

class FrameSync {
public:
    void init(const vk::raii::Device &device, uint32_t maxFramesInFlight);
    void recreateSwapchainSync(const vk::raii::Device &device, size_t swapchainImageCount);
    void cleanup();

    uint32_t getCurrentFrame() const {
        return m_currentFrame;
    }
    void advanceFrame(uint32_t maxFramesInFlight) {
        m_currentFrame = (m_currentFrame + 1) % maxFramesInFlight;
    }

    vk::Fence getCurrentFrameFence() const {
        return *m_inFlightFences[m_currentFrame];
    }
    void recreateCurrentFrameFenceSignaled(const vk::raii::Device &device) {
        vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};
        m_inFlightFences[m_currentFrame] = vk::raii::Fence(device, fenceInfo);
    }
    vk::Semaphore getCurrentImageAvailableSemaphore() const {
        return *m_imageAvailableSemaphores[m_currentFrame];
    }
    vk::Semaphore getRenderFinishedSemaphore(uint32_t imageIndex) const {
        return *m_renderFinishedSemaphores[imageIndex];
    }

    vk::Fence getImageInFlightFence(uint32_t imageIndex) const {
        return m_imagesInFlight[imageIndex];
    }
    void setImageInFlightFence(uint32_t imageIndex, vk::Fence fence) {
        m_imagesInFlight[imageIndex] = fence;
    }

private:
    std::vector<vk::raii::Semaphore> m_imageAvailableSemaphores;
    std::vector<vk::raii::Semaphore> m_renderFinishedSemaphores;
    std::vector<vk::raii::Fence> m_inFlightFences;
    std::vector<vk::Fence> m_imagesInFlight;
    uint32_t m_currentFrame = 0;
};
