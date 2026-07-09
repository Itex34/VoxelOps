#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"

#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"
#include "../../../../Shared/runtime/Paths.hpp"

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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
    static_assert(sizeof(IndexedIndirectCommand) == sizeof(vk::DrawIndexedIndirectCommand));
    static_assert(alignof(IndexedIndirectCommand) == alignof(vk::DrawIndexedIndirectCommand));
} // namespace

VulkanRenderer::VulkanRenderer(VulkanContext &context)
    : m_context(context)
    , m_nrdBootstrap(std::make_unique<NrdBootstrap>()) {}

VulkanRenderer::~VulkanRenderer() noexcept {
    try {
        cleanup();
    } catch (const std::exception &e) {
        std::cerr << "VulkanRenderer cleanup failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "VulkanRenderer cleanup failed with unknown exception.\n";
    }
}

vk::RenderPass VulkanRenderer::getRenderPassHandle() const noexcept {
    if (m_compositeRenderPass.get() == nullptr) {
        return VK_NULL_HANDLE;
    }
    return *m_compositeRenderPass.get();
}

uint32_t VulkanRenderer::getSwapchainImageCount() const noexcept {
    return static_cast<uint32_t>(m_framebuffers.size());
}

bool VulkanRenderer::isNrdBootstrapActive() const noexcept {
    return (m_nrdBootstrap != nullptr) && m_nrdBootstrap->isActive();
}

uint32_t VulkanRenderer::getNrdBootstrapDispatchCount() const noexcept {
    return (m_nrdBootstrap != nullptr) ? m_nrdBootstrap->lastDispatchCount() : 0u;
}

void VulkanRenderer::init() {
    if (m_initialized) {
        cleanup();
    }

    createCommandPool();

    const vk::raii::Device &device = m_context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context.getPhysicalDevice();
    const vk::raii::Queue &graphicsQueue = m_context.getGraphicsQueue();

    m_uploadContext.init(
        device, m_context.getVmaAllocator(), m_context.getGraphicsQueueFamily(), graphicsQueue
    );

    m_fallbackArrayTexture.initFromAtlasFileAsArray(
        device,
        physicalDevice,
        m_uploadContext,
        "",
        1,
        m_context.isSamplerAnisotropyEnabled(),
        m_context.getMaxSamplerAnisotropy()
    );
    m_fallback2DTexture.initFromFile(
        device,
        physicalDevice,
        m_uploadContext,
        "",
        m_context.isSamplerAnisotropyEnabled(),
        m_context.getMaxSamplerAnisotropy()
    );
    m_giBlueNoiseTexture.initFromFile(
        device,
        physicalDevice,
        m_uploadContext,
        Shared::RuntimePaths::ResolveVoxelOpsPath("assets/textures/bluenoise256.exr").generic_string(),
        false,
        1.0f
    );

    m_uploadContext.waitIdle();

    m_frameSync.init(device, MAX_FRAMES_IN_FLIGHT);
    if (m_nrdBootstrap != nullptr) {
        m_nrdBootstrap->init();
    }
    recreateSwapchainDependentResources();

    m_initialized = true;
}

void VulkanRenderer::cleanup() {
    const vk::raii::Device &device = m_context.getDevice();
    try {
        device.waitIdle();
    } catch (const std::exception &e) {
        std::cerr << "VulkanRenderer::cleanup waitIdle failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "VulkanRenderer::cleanup waitIdle failed with unknown exception.\n";
    }

    cleanupSwapchainDependentResources();
    m_frameSync.cleanup();
    if (m_nrdBootstrap != nullptr) {
        m_nrdBootstrap->shutdown();
    }
    m_fallbackArrayTexture.cleanup();
    m_fallback2DTexture.cleanup();
    m_giBlueNoiseTexture.cleanup();
    m_uploadContext.cleanup();

    m_nrdComputeCommandPool.clear();
    m_commandPool.clear();
    m_initialized = false;
}

void VulkanRenderer::createCommandPool() {
    const vk::raii::Device &device = m_context.getDevice();
    if (m_commandPool != nullptr) {
        return;
    }

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_context.getGraphicsQueueFamily();
    m_commandPool = vk::raii::CommandPool(device, poolInfo);
}
