#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan_raii.hpp>
#include <optional>
#include <vector>
#include <cstdint>



struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;

    bool isComplete() const{
        return graphics.has_value() && present.has_value();
    }
};


class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    void init(SDL_Window* window);
    void cleanup();
    bool handleWindowResize(uint32_t windowWidth, uint32_t windowHeight);
    bool shouldRecreateSwapchain(vk::Result result) const;
    const vk::raii::Instance& getInstance() const { return instance; }
    const vk::raii::Device& getDevice() const { return device; }
    const vk::raii::SwapchainKHR& getSwapchain() const { return swapchain; }
    const std::vector<vk::raii::ImageView>& getSwapchainImageViews() const { return swapchainImageViews; }
    vk::Format getSwapchainImageFormat() const { return swapchainImageFormat; }
    vk::Extent2D getSwapchainExtent() const { return swapchainExtent; }
    const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }
    vk::Format getDepthFormat() const { return depthFormat; }
    const vk::raii::Queue& getGraphicsQueue() const { return graphicsQueue; }
    const vk::raii::Queue& getPresentQueue() const { return presentQueue; }
    uint32_t getGraphicsQueueFamily() const { return graphicsQueueFamily; }
    uint32_t getPresentQueueFamily() const { return presentQueueFamily; }
    const vk::raii::PhysicalDevice& getPhysicalDevice() const { return physicalDevice; }
    bool isSamplerAnisotropyEnabled() const { return m_samplerAnisotropyEnabled; }
    float getMaxSamplerAnisotropy() const { return m_maxSamplerAnisotropy; }
    bool areTimestampQueriesSupported() const { return m_timestampQueriesSupported; }
    float getTimestampPeriodNanoseconds() const { return m_timestampPeriodNanoseconds; }
    bool isMultiDrawIndirectEnabled() const { return m_multiDrawIndirectEnabled; }
    bool isDrawIndirectFirstInstanceEnabled() const { return m_drawIndirectFirstInstanceEnabled; }
    bool isHardwareRayTracingSupported() const { return m_hardwareRayTracingSupported; }
    bool isComputeShaderDerivativesEnabled() const { return m_computeShaderDerivativesEnabled; }

private:
    vk::raii::Context context{};
    vk::raii::Instance instance{ nullptr };
    vk::raii::DebugUtilsMessengerEXT debugMessenger{ nullptr };
    vk::raii::Device device{ nullptr };
    vk::raii::PhysicalDevice physicalDevice{ nullptr };
    vk::raii::SurfaceKHR surface{ nullptr };

    vk::raii::Queue graphicsQueue{ nullptr };
    vk::raii::Queue presentQueue{ nullptr };
    uint32_t graphicsQueueFamily = 0;
    uint32_t presentQueueFamily = 0;


    vk::raii::SwapchainKHR swapchain{ nullptr };
    std::vector<vk::Image> swapchainImages;
    std::vector<vk::raii::ImageView> swapchainImageViews;
    vk::Format swapchainImageFormat;
    vk::Extent2D swapchainExtent;
    vk::raii::Image depthImage{ nullptr };
    vk::raii::DeviceMemory depthImageMemory{ nullptr };
    vk::raii::ImageView depthImageView{ nullptr };
    vk::Format depthFormat = vk::Format::eUndefined;

    const std::vector<const char*> m_validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };
    bool m_validationEnabled = false;
    bool m_samplerAnisotropyEnabled = false;
    float m_maxSamplerAnisotropy = 1.0f;
    bool m_timestampQueriesSupported = false;
    float m_timestampPeriodNanoseconds = 0.0f;
    bool m_multiDrawIndirectEnabled = false;
    bool m_drawIndirectFirstInstanceEnabled = false;
    bool m_hardwareRayTracingSupported = false;
    bool m_computeShaderDerivativesEnabled = false;

    const std::vector<const char*> m_deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    void createInstance();
    void createDebugMessenger();
    void pickPhysicalDevice();
    void createDevice();
    void createSurface(SDL_Window* window);
	void createSwapchain(uint32_t windowWidth, uint32_t windowHeight);
    void createSwapchainImageViews();
    void createDepthResources();
    void cleanupSwapchainResources();
    bool recreateSwapchain(uint32_t windowWidth, uint32_t windowHeight);

    vk::raii::ImageView createImageView(vk::Image image, vk::Format format, vk::ImageAspectFlags aspectFlags);
    void createImage(
        uint32_t width,
        uint32_t height,
        vk::Format format,
        vk::ImageTiling tiling,
        vk::ImageUsageFlags usage,
        vk::MemoryPropertyFlags properties,
        vk::raii::Image& image,
        vk::raii::DeviceMemory& imageMemory
    );
    uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
    vk::Format findSupportedFormat(
        const std::vector<vk::Format>& candidates,
        vk::ImageTiling tiling,
        vk::FormatFeatureFlags features
    ) const;
    vk::Format findDepthFormat() const;
    static bool hasStencilComponent(vk::Format format);


    void validateInitPreconditions(SDL_Window* window) const;

    bool checkDeviceExtensionSupport(const vk::raii::PhysicalDevice& device);
    bool checkValidationLayerSupport();
    bool hasAdequateSwapchainSupport(const vk::raii::PhysicalDevice& device, const vk::raii::SurfaceKHR& surface);

    QueueFamilyIndices findQueueFamilies(const vk::raii::PhysicalDevice& device, const vk::raii::SurfaceKHR& surface);
};
