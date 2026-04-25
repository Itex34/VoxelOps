#include "graphics/Vulkan/vulkan/VulkanContext.hpp"

#include <set>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <algorithm>
#include <limits>
#include <sstream>

namespace {
VKAPI_ATTR vk::Bool32 VKAPI_PTR debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity, vk::DebugUtilsMessageTypeFlagsEXT,
    const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData, void *) {
    const char *severity = "INFO";
    if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
        severity = "ERROR";
    else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
        severity = "WARN";
    else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)
        severity = "INFO";
    else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose)
        severity = "VERBOSE";

    const char *message = (pCallbackData && pCallbackData->pMessage) ? pCallbackData->pMessage
                                                                     : "No validation message.";
    std::cerr << "[Vulkan][" << severity << "] " << message << "\n";
    return vk::False;
}

vk::DebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo() {
    vk::DebugUtilsMessengerCreateInfoEXT info{};
    info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    info.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    info.pfnUserCallback = debugCallback;
    return info;
}

vk::Extent2D chooseSwapchainExtent(const vk::SurfaceCapabilitiesKHR &capabilities,
                                   uint32_t windowWidth, uint32_t windowHeight) {
    vk::Extent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(windowWidth, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(windowHeight, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }
    return extent;
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool tryParseBoolEnv(const char *name, bool &outValue) {
    const char *env = std::getenv(name);
    if (env == nullptr) {
        return false;
    }

    const std::string value = toLowerCopy(std::string(env));
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        outValue = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        outValue = false;
        return true;
    }
    return false;
}

bool isBenchmarkModeEnabled() {
    bool enabled = false;
    return tryParseBoolEnv("VOXELOPS_BENCHMARK", enabled) && enabled;
}

const char *presentModeToString(vk::PresentModeKHR mode) {
    switch (mode) {
    case vk::PresentModeKHR::eImmediate:
        return "immediate";
    case vk::PresentModeKHR::eMailbox:
        return "mailbox";
    case vk::PresentModeKHR::eFifo:
        return "fifo";
    case vk::PresentModeKHR::eFifoRelaxed:
        return "fifo_relaxed";
    default:
        return "other";
    }
}

vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR> &presentModes) {
    const bool benchmarkMode = isBenchmarkModeEnabled();
    vk::PresentModeKHR preferredMode =
        benchmarkMode ? vk::PresentModeKHR::eImmediate : vk::PresentModeKHR::eMailbox;

    if (const char *env = std::getenv("VOXELOPS_VK_PRESENT_MODE")) {
        std::string mode = toLowerCopy(std::string(env));
        if (mode == "immediate") {
            preferredMode = vk::PresentModeKHR::eImmediate;
        } else if (mode == "fifo") {
            preferredMode = vk::PresentModeKHR::eFifo;
        } else if (mode == "mailbox") {
            preferredMode = vk::PresentModeKHR::eMailbox;
        }
    }

    auto hasMode = [&presentModes](vk::PresentModeKHR target) {
        return std::find(presentModes.begin(), presentModes.end(), target) != presentModes.end();
    };

    if (hasMode(preferredMode)) {
        return preferredMode;
    }
    if (hasMode(vk::PresentModeKHR::eMailbox)) {
        return vk::PresentModeKHR::eMailbox;
    }
    if (hasMode(vk::PresentModeKHR::eImmediate)) {
        return vk::PresentModeKHR::eImmediate;
    }
    return vk::PresentModeKHR::eFifo;
}

bool shouldEnableValidationLayers() {
#ifdef NDEBUG
    bool enabledByDefault = false;
#else
    bool enabledByDefault = true;
#endif

    bool explicitValidation = false;
    if (tryParseBoolEnv("VOXELOPS_VK_VALIDATION", explicitValidation)) {
        return explicitValidation;
    }

    if (isBenchmarkModeEnabled()) {
        return false;
    }

    return enabledByDefault;
}
} // namespace

VulkanContext::~VulkanContext() {
    try {
        cleanup();
    } catch (...) {
    }
}

void VulkanContext::init(SDL_Window *window) {
    validateInitPreconditions(window);

    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);

    try {
        createInstance();
        createDebugMessenger();
        createSurface(window);
        pickPhysicalDevice();
        createDevice();
        createSwapchain(w, h);
        createSwapchainImageViews();
        createDepthResources();
    } catch (...) {
        cleanup();
        throw;
    }
}

void VulkanContext::cleanup() {
    if (device != nullptr) {
        try {
            device.waitIdle();
        } catch (...) {
        }
    }

    cleanupSwapchainResources();
    device.clear();
    surface.clear();
    physicalDevice.clear();
    debugMessenger.clear();
    instance.clear();
    graphicsQueue = vk::raii::Queue(nullptr);
    presentQueue = vk::raii::Queue(nullptr);
    graphicsQueueFamily = 0;
    presentQueueFamily = 0;
    m_samplerAnisotropyEnabled = false;
    m_maxSamplerAnisotropy = 1.0f;
    m_timestampQueriesSupported = false;
    m_timestampPeriodNanoseconds = 0.0f;
    m_multiDrawIndirectEnabled = false;
    m_drawIndirectFirstInstanceEnabled = false;
    m_hardwareRayTracingSupported = false;
    m_computeShaderDerivativesEnabled = false;
}

bool VulkanContext::handleWindowResize(uint32_t windowWidth, uint32_t windowHeight) {
    return recreateSwapchain(windowWidth, windowHeight);
}

bool VulkanContext::shouldRecreateSwapchain(vk::Result result) const {
    return result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR;
}

void VulkanContext::validateInitPreconditions(SDL_Window *window) const {
    if (!window) {
        throw std::runtime_error("VulkanContext::init requires a valid SDL window.");
    }

    if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
        throw std::runtime_error(
            "SDL video subsystem is not initialized. Call SDL_Init(SDL_INIT_VIDEO) first.");
    }
}

bool VulkanContext::checkValidationLayerSupport() {
    auto availableLayers = vk::enumerateInstanceLayerProperties();

    for (const char *layerName : m_validationLayers) {
        bool found = false;

        for (const auto &layer : availableLayers) {
            if (strcmp(layer.layerName, layerName) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            return false;
        }
    }

    return true;
}

bool VulkanContext::checkDeviceExtensionSupport(const vk::raii::PhysicalDevice &device) {
    auto availableExtensions = device.enumerateDeviceExtensionProperties();

    std::set<std::string> required(m_deviceExtensions.begin(), m_deviceExtensions.end());
    for (const auto &ext : availableExtensions) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

void VulkanContext::createInstance() {
    vk::ApplicationInfo appInfo("Vulkan SDL3", 1, "No Engine", 1, VK_API_VERSION_1_3);

    uint32_t extensionCount = 0;
    const char *const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);

    if (!sdlExtensions) {
        throw std::runtime_error(SDL_GetError());
    }

    std::vector<const char *> extensions(sdlExtensions, sdlExtensions + extensionCount);

    std::vector<const char *> layers = m_validationLayers;

    const bool wantValidation = shouldEnableValidationLayers();
    m_validationEnabled = wantValidation && checkValidationLayerSupport();
    if (!m_validationEnabled) {
        layers.clear();
        if (wantValidation) {
            std::cerr << "Validation layer VK_LAYER_KHRONOS_validation not found. Continuing "
                         "without validation.\n";
        }
    } else {
        if (std::find(extensions.begin(), extensions.end(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ==
            extensions.end()) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    vk::InstanceCreateInfo createInfo({}, &appInfo, static_cast<uint32_t>(layers.size()),
                                      layers.data(), static_cast<uint32_t>(extensions.size()),
                                      extensions.data());

    vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (m_validationEnabled) {
        debugCreateInfo = makeDebugMessengerCreateInfo();
        createInfo.pNext = &debugCreateInfo;
    }

    instance = vk::raii::Instance(context, createInfo);
}

void VulkanContext::createDebugMessenger() {
    if (!m_validationEnabled) {
        return;
    }

    vk::DebugUtilsMessengerCreateInfoEXT info = makeDebugMessengerCreateInfo();
    debugMessenger = vk::raii::DebugUtilsMessengerEXT(instance, info);
}

void VulkanContext::createSurface(SDL_Window *window) {
    VkSurfaceKHR c_surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(*instance), nullptr,
                                  &c_surface)) {
        throw std::runtime_error(std::string("Failed to create Vulkan surface from SDL window: ") +
                                 SDL_GetError());
    }

    surface = vk::raii::SurfaceKHR(instance, c_surface);
}

void VulkanContext::pickPhysicalDevice() {
    auto devices = instance.enumeratePhysicalDevices();
    if (devices.empty()) {
        throw std::runtime_error("No Vulkan-capable GPU found.");
    }

    vk::raii::PhysicalDevice bestDevice = nullptr;
    int bestScore = -1;
    std::vector<std::string> missingSwapchainExtensionDevices;

    for (const auto &dev : devices) {
        auto indices = findQueueFamilies(dev, surface);
        bool extensionsSupported = checkDeviceExtensionSupport(dev);
        bool swapchainAdequate = extensionsSupported && hasAdequateSwapchainSupport(dev, surface);

        if (!extensionsSupported) {
            missingSwapchainExtensionDevices.emplace_back(dev.getProperties().deviceName.data());
        }

        if (!indices.isComplete() || !extensionsSupported || !swapchainAdequate) {
            continue;
        }

        int score = 0;

        vk::PhysicalDeviceProperties props = dev.getProperties();

        if (props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu) {
            score += 1000;
        }

        score += props.limits.maxImageDimension2D;

        if (score > bestScore) {
            bestScore = score;
            bestDevice = dev;
        }
    }

    if (bestDevice == nullptr) {
        if (!missingSwapchainExtensionDevices.empty()) {
            std::ostringstream oss;
            oss << "No suitable GPU found. Required device extension "
                << VK_KHR_SWAPCHAIN_EXTENSION_NAME << " is missing on: ";
            for (size_t i = 0; i < missingSwapchainExtensionDevices.size(); ++i) {
                if (i > 0) {
                    oss << ", ";
                }
                oss << missingSwapchainExtensionDevices[i];
            }
            throw std::runtime_error(oss.str());
        }
        throw std::runtime_error("No suitable GPU found.");
    }

    physicalDevice = bestDevice;
}

bool VulkanContext::hasAdequateSwapchainSupport(const vk::raii::PhysicalDevice &device,
                                                const vk::raii::SurfaceKHR &surface) {
    auto formats = device.getSurfaceFormatsKHR(surface);
    auto presentModes = device.getSurfacePresentModesKHR(surface);
    return !formats.empty() && !presentModes.empty();
}

QueueFamilyIndices VulkanContext::findQueueFamilies(const vk::raii::PhysicalDevice &device,
                                                    const vk::raii::SurfaceKHR &surface) {
    QueueFamilyIndices indices;
    auto families = device.getQueueFamilyProperties();

    for (uint32_t i = 0; i < families.size(); i++) {
        if (!indices.graphics && (families[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
            indices.graphics = i;
        }

        if (!indices.present && device.getSurfaceSupportKHR(i, surface)) {
            indices.present = i;
        }

        if (indices.isComplete()) {
            break;
        }
    }

    return indices;
}

void VulkanContext::createDevice() {
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);
    if (!indices.isComplete()) {
        throw std::runtime_error("Queue families not complete.");
    }

    float priority = 1.0f;
    std::vector<vk::DeviceQueueCreateInfo> queueInfos;

    std::set<uint32_t> uniqueQueues = {indices.graphics.value(), indices.present.value()};

    for (uint32_t queueFamily : uniqueQueues) {
        queueInfos.emplace_back(vk::DeviceQueueCreateInfo({}, queueFamily, 1, &priority));
    }

    vk::PhysicalDeviceFeatures supportedFeatures = physicalDevice.getFeatures();
    const auto queueFamilies = physicalDevice.getQueueFamilyProperties();
    vk::PhysicalDeviceFeatures features{};
    if (supportedFeatures.samplerAnisotropy == VK_TRUE) {
        features.samplerAnisotropy = VK_TRUE;
        m_samplerAnisotropyEnabled = true;
    } else {
        m_samplerAnisotropyEnabled = false;
    }
    m_maxSamplerAnisotropy = physicalDevice.getProperties().limits.maxSamplerAnisotropy;
    if (indices.graphics.value() < queueFamilies.size() &&
        queueFamilies[indices.graphics.value()].timestampValidBits > 0) {
        m_timestampQueriesSupported = true;
        m_timestampPeriodNanoseconds = physicalDevice.getProperties().limits.timestampPeriod;
    } else {
        m_timestampQueriesSupported = false;
        m_timestampPeriodNanoseconds = 0.0f;
    }
    if (supportedFeatures.multiDrawIndirect == VK_TRUE) {
        features.multiDrawIndirect = VK_TRUE;
        m_multiDrawIndirectEnabled = true;
    } else {
        m_multiDrawIndirectEnabled = false;
    }
    if (supportedFeatures.drawIndirectFirstInstance == VK_TRUE) {
        features.drawIndirectFirstInstance = VK_TRUE;
        m_drawIndirectFirstInstanceEnabled = true;
    } else {
        m_drawIndirectFirstInstanceEnabled = false;
    }
    if (supportedFeatures.fragmentStoresAndAtomics == VK_TRUE) {
        features.fragmentStoresAndAtomics = VK_TRUE;
    } else {
        std::cerr << "[Vulkan] fragmentStoresAndAtomics not supported; fragment storage image "
                     "writes are unavailable.\n";
    }

    std::vector<const char *> enabledDeviceExtensions = m_deviceExtensions;
    auto availableExtensions = physicalDevice.enumerateDeviceExtensionProperties();
    std::set<std::string> availableExtensionNames;
    for (const auto &ext : availableExtensions) {
        availableExtensionNames.insert(ext.extensionName);
    }
    const auto hasExtension = [&availableExtensionNames](const char *name) {
        return availableExtensionNames.find(name) != availableExtensionNames.end();
    };

#if defined(VK_KHR_compute_shader_derivatives)
    const bool computeDerivativesExtensionAvailable =
        hasExtension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
#else
    const bool computeDerivativesExtensionAvailable = false;
#endif

    const bool rtExtensionsAvailable = hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                                       hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                                       hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    VkPhysicalDeviceFeatures2 supportedFeatures2{};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan12Features supportedVulkan12{};
    supportedVulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAccelerationStructure{};
    supportedAccelerationStructure.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceRayQueryFeaturesKHR supportedRayQuery{};
    supportedRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
#if defined(VK_KHR_compute_shader_derivatives)
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR supportedComputeDerivatives{};
    supportedComputeDerivatives.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;
#endif
    supportedFeatures2.pNext = &supportedVulkan12;
    supportedVulkan12.pNext = &supportedAccelerationStructure;
    supportedAccelerationStructure.pNext = &supportedRayQuery;
#if defined(VK_KHR_compute_shader_derivatives)
    supportedRayQuery.pNext = &supportedComputeDerivatives;
#endif
    vkGetPhysicalDeviceFeatures2(static_cast<VkPhysicalDevice>(*physicalDevice),
                                 &supportedFeatures2);

    const bool rtFeatureSupport =
        (supportedVulkan12.bufferDeviceAddress == VK_TRUE) &&
        (supportedAccelerationStructure.accelerationStructure == VK_TRUE) &&
        (supportedRayQuery.rayQuery == VK_TRUE);
    m_hardwareRayTracingSupported = rtExtensionsAvailable && rtFeatureSupport;
    std::cout << "[Vulkan] hardware ray tracing support="
              << (m_hardwareRayTracingSupported ? "enabled" : "unavailable")
              << " (extensions=" << (rtExtensionsAvailable ? "ok" : "missing")
              << ", features=" << (rtFeatureSupport ? "ok" : "missing") << ")\n";
    if (m_hardwareRayTracingSupported) {
        enabledDeviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        enabledDeviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        enabledDeviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }

    bool computeDerivativeQuadsFeatureSupported = false;
#if defined(VK_KHR_compute_shader_derivatives)
    computeDerivativeQuadsFeatureSupported =
        (supportedComputeDerivatives.computeDerivativeGroupQuads == VK_TRUE);
#endif
    m_computeShaderDerivativesEnabled =
        computeDerivativesExtensionAvailable && computeDerivativeQuadsFeatureSupported;
    std::cout << "[Vulkan] compute shader derivatives support="
              << (m_computeShaderDerivativesEnabled ? "enabled" : "unavailable")
              << " (extension=" << (computeDerivativesExtensionAvailable ? "ok" : "missing")
              << ", quads=" << (computeDerivativeQuadsFeatureSupported ? "ok" : "missing") << ")\n";
    if (m_computeShaderDerivativesEnabled) {
#if defined(VK_KHR_compute_shader_derivatives)
        enabledDeviceExtensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
#endif
    }

    VkPhysicalDeviceFeatures2 enabledFeatures2{};
    enabledFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures2.features = features;
    VkPhysicalDeviceVulkan12Features enabledVulkan12{};
    enabledVulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAccelerationStructure{};
    enabledAccelerationStructure.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceRayQueryFeaturesKHR enabledRayQuery{};
    enabledRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    enabledFeatures2.pNext = nullptr;
    if (m_hardwareRayTracingSupported) {
        enabledFeatures2.pNext = &enabledVulkan12;
        enabledVulkan12.pNext = &enabledAccelerationStructure;
        enabledAccelerationStructure.pNext = &enabledRayQuery;
        enabledRayQuery.pNext = nullptr;

        enabledVulkan12.bufferDeviceAddress = VK_TRUE;
        enabledAccelerationStructure.accelerationStructure = VK_TRUE;
        enabledRayQuery.rayQuery = VK_TRUE;
    }
#if defined(VK_KHR_compute_shader_derivatives)
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR enabledComputeDerivatives{};
    enabledComputeDerivatives.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;
    enabledComputeDerivatives.pNext = nullptr;
    if (m_computeShaderDerivativesEnabled) {
        enabledComputeDerivatives.computeDerivativeGroupQuads = VK_TRUE;
        if (supportedComputeDerivatives.computeDerivativeGroupLinear == VK_TRUE) {
            enabledComputeDerivatives.computeDerivativeGroupLinear = VK_TRUE;
        }
        if (m_hardwareRayTracingSupported) {
            enabledRayQuery.pNext = &enabledComputeDerivatives;
        } else {
            enabledFeatures2.pNext = &enabledComputeDerivatives;
        }
    }
#endif

    vk::DeviceCreateInfo deviceInfo{};
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
    deviceInfo.pEnabledFeatures = nullptr;
    deviceInfo.pNext = &enabledFeatures2;

    device = vk::raii::Device(physicalDevice, deviceInfo);
    graphicsQueue = device.getQueue(indices.graphics.value(), 0);
    presentQueue = device.getQueue(indices.present.value(), 0);
    graphicsQueueFamily = indices.graphics.value();
    presentQueueFamily = indices.present.value();
}

void VulkanContext::createSwapchain(uint32_t windowWidth, uint32_t windowHeight) {
    vk::SurfaceCapabilitiesKHR capabilities = physicalDevice.getSurfaceCapabilitiesKHR(surface);

    auto formats = physicalDevice.getSurfaceFormatsKHR(surface);
    if (formats.empty()) {
        throw std::runtime_error("No surface formats available.");
    }

    vk::SurfaceFormatKHR chosenFormat = formats[0];
    for (const auto &f : formats) {
        if (f.format == vk::Format::eB8G8R8A8Srgb &&
            f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            chosenFormat = f;
            break;
        }
    }

    auto presentModes = physicalDevice.getSurfacePresentModesKHR(surface);
    if (presentModes.empty()) {
        throw std::runtime_error("No present modes available.");
    }

    vk::PresentModeKHR presentMode = choosePresentMode(presentModes);

    const vk::Extent2D extent = chooseSwapchainExtent(capabilities, windowWidth, windowHeight);
    if (extent.width == 0 || extent.height == 0) {
        throw std::runtime_error("Swapchain extent is zero.");
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);
    uint32_t queueFamilyIndices[] = {indices.graphics.value(), indices.present.value()};

    vk::SwapchainCreateInfoKHR info{};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosenFormat.format;
    info.imageColorSpace = chosenFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;

    if (indices.graphics != indices.present) {
        info.imageSharingMode = vk::SharingMode::eConcurrent;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        info.imageSharingMode = vk::SharingMode::eExclusive;
    }

    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = nullptr;

    swapchain = vk::raii::SwapchainKHR(device, info);
    swapchainImages = swapchain.getImages();
    swapchainImageFormat = chosenFormat.format;
    swapchainExtent = extent;
    std::cout << "[Vulkan] Swapchain present mode: " << presentModeToString(presentMode)
              << " | images: " << imageCount << " | extent: " << extent.width << "x"
              << extent.height << "\n";
}

void VulkanContext::createSwapchainImageViews() {
    swapchainImageViews.clear();
    swapchainImageViews.reserve(swapchainImages.size());

    for (vk::Image image : swapchainImages) {
        swapchainImageViews.emplace_back(
            createImageView(image, swapchainImageFormat, vk::ImageAspectFlagBits::eColor));
    }
}

void VulkanContext::createDepthResources() {
    depthFormat = findDepthFormat();
    createImage(swapchainExtent.width, swapchainExtent.height, depthFormat,
                vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                vk::MemoryPropertyFlagBits::eDeviceLocal, depthImage, depthImageMemory);

    vk::ImageAspectFlags aspectFlags = vk::ImageAspectFlagBits::eDepth;
    if (hasStencilComponent(depthFormat)) {
        aspectFlags |= vk::ImageAspectFlagBits::eStencil;
    }
    depthImageView = createImageView(*depthImage, depthFormat, aspectFlags);
}

void VulkanContext::cleanupSwapchainResources() {
    depthImageView.clear();
    depthImage.clear();
    depthImageMemory.clear();
    depthFormat = vk::Format::eUndefined;
    swapchainImageViews.clear();
    swapchainImages.clear();
    swapchain.clear();
}

bool VulkanContext::recreateSwapchain(uint32_t windowWidth, uint32_t windowHeight) {
    if (device == nullptr || windowWidth == 0 || windowHeight == 0) {
        return false;
    }

    const vk::SurfaceCapabilitiesKHR capabilities =
        physicalDevice.getSurfaceCapabilitiesKHR(surface);
    const vk::Extent2D extent = chooseSwapchainExtent(capabilities, windowWidth, windowHeight);
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    try {
        device.waitIdle();
        cleanupSwapchainResources();
        createSwapchain(windowWidth, windowHeight);
        createSwapchainImageViews();
        createDepthResources();
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] recreateSwapchain failed: " << e.what() << "\n";
        return false;
    }
}

vk::raii::ImageView VulkanContext::createImageView(vk::Image image, vk::Format format,
                                                   vk::ImageAspectFlags aspectFlags) {
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    return vk::raii::ImageView(device, viewInfo);
}

void VulkanContext::createImage(uint32_t width, uint32_t height, vk::Format format,
                                vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                                vk::MemoryPropertyFlags properties, vk::raii::Image &image,
                                vk::raii::DeviceMemory &imageMemory) {
    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent = vk::Extent3D(width, height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = usage;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    image = vk::raii::Image(device, imageInfo);
    vk::MemoryRequirements memoryRequirements = image.getMemoryRequirements();

    vk::MemoryAllocateInfo allocateInfo{};
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

    imageMemory = vk::raii::DeviceMemory(device, allocateInfo);
    image.bindMemory(*imageMemory, 0);
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter,
                                       vk::MemoryPropertyFlags properties) const {
    vk::PhysicalDeviceMemoryProperties memoryProperties = physicalDevice.getMemoryProperties();

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeMatches = (typeFilter & (1u << i)) != 0;
        const bool propertyMatches =
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && propertyMatches) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type for image allocation.");
}

vk::Format VulkanContext::findSupportedFormat(const std::vector<vk::Format> &candidates,
                                              vk::ImageTiling tiling,
                                              vk::FormatFeatureFlags features) const {
    for (vk::Format format : candidates) {
        vk::FormatProperties props = physicalDevice.getFormatProperties(format);
        if (tiling == vk::ImageTiling::eLinear &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == vk::ImageTiling::eOptimal &&
            (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    throw std::runtime_error("Failed to find a supported depth format.");
}

vk::Format VulkanContext::findDepthFormat() const {
    return findSupportedFormat(
        {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
        vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

bool VulkanContext::hasStencilComponent(vk::Format format) {
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
}
