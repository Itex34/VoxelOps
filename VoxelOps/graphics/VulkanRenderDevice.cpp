#include "VulkanRenderDevice.hpp"

#include "Camera.hpp"
#include "Frustum.hpp"
#include "Renderer.hpp"
#include "ISkyBackend.hpp"
#include "Vulkan/renderer/VulkanRenderer.hpp"
#include "Vulkan/vulkan/VulkanContext.hpp"
#include "Vulkan/vulkan/VulkanUtils.hpp"
#include "data/GameData.hpp"
#include "voxels/Voxel.hpp"
#include "../../Shared/player/PlayerData.hpp"
#include "../player/Player.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace {
constexpr float kGiTemporalBlend = 0.14f;
constexpr uint32_t kGiOccupancyPaddingMultiplier = 2;
constexpr float kGiLumaFixedScale = 100000.0f;
constexpr bool kEnablePathTracedGi = true;
constexpr uint32_t kPathTraceRaysPerPixel = 1u;
constexpr uint32_t kPathTraceMaxBounces = 2u;
constexpr float kPathTraceSkyIntensity = 1.0f;
constexpr glm::ivec3 kRtDummyChunkPos{250000, 250000, 250000};

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isSolidBlock(BlockID id) {
    if (id == BlockID::Leaves) {
        return true;
    }
    const auto it = blockTypes.find(id);
    if (it != blockTypes.end()) {
        return it->second.isSolid;
    }
    return id != BlockID::Air;
}

vk::raii::ShaderModule loadShaderModule(const vk::raii::Device &device, const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open shader: " + path);
    }

    const size_t size = static_cast<size_t>(file.tellg());
    if (size == 0 || (size % 4) != 0) {
        throw std::runtime_error("Invalid shader size: " + path);
    }

    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("Failed to read shader: " + path);
    }

    vk::ShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.codeSize = size;
    shaderModuleInfo.pCode = code.data();
    return vk::raii::ShaderModule(device, shaderModuleInfo);
}

inline void setOccupancyBit(std::vector<uint32_t> &words, uint32_t linearIndex) {
    const uint32_t wordIndex = linearIndex >> 5u;
    const uint32_t bit = linearIndex & 31u;
    if (wordIndex < words.size()) {
        words[wordIndex] |= (1u << bit);
    }
}

struct DeviceBufferAllocation {
    vk::raii::Buffer buffer{nullptr};
    vk::raii::DeviceMemory memory{nullptr};

    void reset() {
        buffer.clear();
        memory.clear();
    }
};

void createBufferWithAddressing(const vk::raii::Device &device,
                                const vk::raii::PhysicalDevice &physicalDevice, vk::DeviceSize size,
                                vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties,
                                bool enableDeviceAddress, DeviceBufferAllocation &outBuffer) {
    outBuffer.reset();

    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = std::max<vk::DeviceSize>(size, 4u);
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    outBuffer.buffer = vk::raii::Buffer(device, bufferInfo);

    const vk::MemoryRequirements requirements = outBuffer.buffer.getMemoryRequirements();

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex =
        VulkanUtils::findMemoryType(physicalDevice, requirements.memoryTypeBits, properties);

    vk::MemoryAllocateFlagsInfo allocFlags{};
    if (enableDeviceAddress) {
        allocFlags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
        allocInfo.pNext = &allocFlags;
    }

    outBuffer.memory = vk::raii::DeviceMemory(device, allocInfo);
    outBuffer.buffer.bindMemory(*outBuffer.memory, 0);
}

vk::DeviceAddress getBufferDeviceAddress(const vk::raii::Device &device, vk::Buffer buffer) {
    vk::BufferDeviceAddressInfo addressInfo{};
    addressInfo.buffer = buffer;
    return device.getBufferAddress(addressInfo);
}

glm::vec3 decodePackedVoxelPosition(const VoxelVertex &packed) {
    const uint32_t x = (packed.low >> 0u) & 31u;
    const uint32_t y = (packed.low >> 5u) & 31u;
    const uint32_t z = (packed.low >> 10u) & 31u;
    return glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

VkTransformMatrixKHR makeTranslationTransform(const glm::vec3 &translation) {
    VkTransformMatrixKHR out{};
    out.matrix[0][0] = 1.0f;
    out.matrix[0][1] = 0.0f;
    out.matrix[0][2] = 0.0f;
    out.matrix[0][3] = translation.x;
    out.matrix[1][0] = 0.0f;
    out.matrix[1][1] = 1.0f;
    out.matrix[1][2] = 0.0f;
    out.matrix[1][3] = translation.y;
    out.matrix[2][0] = 0.0f;
    out.matrix[2][1] = 0.0f;
    out.matrix[2][2] = 1.0f;
    out.matrix[2][3] = translation.z;
    return out;
}

VoxelVertex makePackedVoxelVertex(uint32_t x, uint32_t y, uint32_t z) {
    VoxelVertex out{};
    out.low = ((x & 31u) << 0u) | ((y & 31u) << 5u) | ((z & 31u) << 10u);
    out.high = 0u;
    return out;
}

struct alignas(16) GpuProbeSample {
    glm::vec4 irradianceDepthMean{0.20f, 0.24f, 0.30f, 1.0f};
    // x = directional strength, y = integrated frames, zw = oct-encoded dominant direction
    glm::vec4 depthMomentFrames{0.0f, 0.0f, 0.5f, 1.0f};
};

struct alignas(16) GpuProbeStats {
    uint32_t lumaSumFixed = 0;
    uint32_t lumaCount = 0;
    uint32_t _pad0 = 0;
    uint32_t _pad1 = 0;
};

struct alignas(16) GiComputePushConstants {
    uint32_t firstProbe = 0;
    uint32_t probesUpdated = 0;
    uint32_t totalProbes = 0;
    uint32_t raysPerProbe = 0;

    int32_t snappedOriginX = 0;
    int32_t snappedOriginY = 0;
    int32_t snappedOriginZ = 0;
    uint32_t spacingBlocks = 1;

    uint32_t probeCountX = 0;
    uint32_t probeCountY = 0;
    uint32_t probeCountZ = 0;
    uint32_t frameIndexLow = 0;

    int32_t occupancyMinX = 0;
    int32_t occupancyMinY = 0;
    int32_t occupancyMinZ = 0;
    uint32_t occupancyDimX = 0;
    uint32_t occupancyDimY = 0;
    uint32_t occupancyDimZ = 0;
    uint32_t occupancyWordCount = 0;

    int32_t worldMinX = 0;
    int32_t worldMaxX = 0;
    int32_t worldMinY = 0;
    int32_t worldMaxY = 0;
    int32_t worldMinZ = 0;
    int32_t worldMaxZ = 0;

    float maxTraceDistance = 1.0f;
    float temporalBlend = kGiTemporalBlend;
    float sunDirX = 0.25f;
    float sunDirY = 0.85f;
    float sunDirZ = 0.42f;
    float sunIntensity = 1.10f;
};
} // namespace

struct VulkanRenderDevice::GiComputeState {
    struct CascadeResources {
        glm::ivec3 occupancyMinBlocks{0};
        glm::uvec3 occupancyDims{0u};
        uint32_t occupancyWordCount = 0;
        std::vector<uint32_t> occupancyWords;
        glm::ivec3 occupancyAnchorSnappedOrigin{std::numeric_limits<int>::min()};
        uint64_t occupancyLastBuildFrame = 0;

        vk::raii::Buffer occupancyBuffer{nullptr};
        vk::raii::DeviceMemory occupancyMemory{nullptr};
        void *occupancyMapped = nullptr;

        std::vector<uint32_t> materialIds;
        vk::raii::Buffer materialBuffer{nullptr};
        vk::raii::DeviceMemory materialMemory{nullptr};
        void *materialMapped = nullptr;

        vk::raii::Buffer probeBuffer{nullptr};
        vk::raii::DeviceMemory probeMemory{nullptr};
        void *probeMapped = nullptr;
        uint32_t probeCount = 0;

        vk::raii::Buffer statsBuffer{nullptr};
        vk::raii::DeviceMemory statsMemory{nullptr};
        void *statsMapped = nullptr;

        void reset() {
            if (occupancyMapped != nullptr) {
                occupancyMemory.unmapMemory();
                occupancyMapped = nullptr;
            }
            if (probeMapped != nullptr) {
                probeMemory.unmapMemory();
                probeMapped = nullptr;
            }
            if (materialMapped != nullptr) {
                materialMemory.unmapMemory();
                materialMapped = nullptr;
            }
            if (statsMapped != nullptr) {
                statsMemory.unmapMemory();
                statsMapped = nullptr;
            }

            occupancyBuffer.clear();
            occupancyMemory.clear();
            materialBuffer.clear();
            materialMemory.clear();
            probeBuffer.clear();
            probeMemory.clear();
            statsBuffer.clear();
            statsMemory.clear();

            occupancyMinBlocks = glm::ivec3(0);
            occupancyDims = glm::uvec3(0u);
            occupancyWordCount = 0;
            occupancyWords.clear();
            materialIds.clear();
            occupancyAnchorSnappedOrigin = glm::ivec3(std::numeric_limits<int>::min());
            occupancyLastBuildFrame = 0;
            probeCount = 0;
        }
    };

    bool ready = false;
    bool timestampEnabled = false;
    float timestampPeriodNs = 0.0f;

    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    std::vector<vk::raii::DescriptorSet> descriptorSets;
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};
    vk::raii::QueryPool queryPool{nullptr};
    std::array<CascadeResources, GiClipmapPlan::MAX_CASCADES> cascades{};

    void reset() {
        for (CascadeResources &cascade : cascades) {
            cascade.reset();
        }
        descriptorSets.clear();
        queryPool.clear();
        fence.clear();
        commandBuffer.clear();
        commandPool.clear();
        pipeline.clear();
        pipelineLayout.clear();
        descriptorPool.clear();
        descriptorSetLayout.clear();
        ready = false;
        timestampEnabled = false;
        timestampPeriodNs = 0.0f;
    }
};

struct VulkanRenderDevice::RtSceneState {
    struct ChunkBlas {
        DeviceBufferAllocation vertexBuffer{};
        DeviceBufferAllocation indexBuffer{};
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        uint64_t revision = 0;
        uint32_t primitiveCount = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            indexBuffer.reset();
            vertexBuffer.reset();
            revision = 0;
            primitiveCount = 0;
        }
    };

    struct RetiredChunkBlas {
        ChunkBlas blas{};
        uint64_t retireFrame = 0;
    };

    struct RetiredTlas {
        DeviceBufferAllocation asBuffer{};
        vk::raii::AccelerationStructureKHR as{nullptr};
        uint64_t retireFrame = 0;

        void reset() {
            as.clear();
            asBuffer.reset();
            retireFrame = 0;
        }
    };

    vk::raii::CommandPool commandPool{nullptr};
    ChunkBlas dummyBlas{};
    std::unordered_map<glm::ivec3, ChunkBlas, IVec3Hash> chunkBlases;
    std::vector<RetiredChunkBlas> retiredChunkBlases;

    DeviceBufferAllocation tlasAsBuffer{};
    vk::raii::AccelerationStructureKHR tlas{nullptr};
    std::vector<RetiredTlas> retiredTlases;

    bool ready = false;

    void reset() {
        tlas.clear();
        tlasAsBuffer.reset();
        for (RetiredTlas &retired : retiredTlases) {
            retired.reset();
        }
        retiredTlases.clear();

        for (RetiredChunkBlas &retired : retiredChunkBlases) {
            retired.blas.reset();
        }
        retiredChunkBlases.clear();

        for (auto &[_, chunkBlas] : chunkBlases) {
            chunkBlas.reset();
        }
        chunkBlases.clear();
        dummyBlas.reset();

        commandPool.clear();
        ready = false;
    }
};

VulkanRenderDevice::VulkanRenderDevice() = default;

VulkanRenderDevice::~VulkanRenderDevice() {
    shutdown();
}

bool VulkanRenderDevice::initialize(SDL_Window *window) {
    if (window == nullptr) {
        std::cerr << "[Vulkan] initialize failed: null SDL window.\n";
        return false;
    }

    try {
        m_window = window;

        m_context = std::make_unique<VulkanContext>();
        m_context->init(m_window);

        m_renderer = std::make_unique<VulkanRenderer>(*m_context);
        m_renderer->init();

        m_uploadContext.init(m_context->getDevice(), m_context->getGraphicsQueueFamily(),
                             m_context->getGraphicsQueue());

        if (!ensureAtlasTextureLoaded()) {
            return false;
        }
        (void)ensureRemotePlayerAssetsLoaded();

        m_uploadContext.waitIdle();
        initGiClipmaps();
        initRayTracingScene();
        m_initialized = true;
        m_warnedUninitializedRender = false;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] initialize failed: " << e.what() << "\n";
        shutdown();
        return false;
    }
}

void VulkanRenderDevice::onWindowResized(int width, int height) {
    if (!m_initialized || !m_renderer) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    try {
        m_renderer->handleWindowResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] handleWindowResize failed: " << e.what() << "\n";
    }
}

VkInstance VulkanRenderDevice::getVkInstanceHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getInstance();
}

VkPhysicalDevice VulkanRenderDevice::getVkPhysicalDeviceHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getPhysicalDevice();
}

VkDevice VulkanRenderDevice::getVkDeviceHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getDevice();
}

uint32_t VulkanRenderDevice::getVkGraphicsQueueFamily() const noexcept {
    if (!m_context) {
        return 0;
    }
    return m_context->getGraphicsQueueFamily();
}

VkQueue VulkanRenderDevice::getVkGraphicsQueueHandle() const noexcept {
    if (!m_context) {
        return VK_NULL_HANDLE;
    }
    return *m_context->getGraphicsQueue();
}

VkRenderPass VulkanRenderDevice::getVkRenderPassHandle() const noexcept {
    if (!m_renderer) {
        return VK_NULL_HANDLE;
    }
    return m_renderer->getRenderPassHandle();
}

uint32_t VulkanRenderDevice::getVkSwapchainImageCount() const noexcept {
    if (!m_renderer) {
        return 0;
    }
    return m_renderer->getSwapchainImageCount();
}

int VulkanRenderDevice::getOpenGLVersionMajor() const noexcept {
    return 0;
}

int VulkanRenderDevice::getOpenGLVersionMinor() const noexcept {
    return 0;
}

GraphicsBackend VulkanRenderDevice::getActiveBackend() const noexcept {
    return GraphicsBackend::Performance;
}

std::string_view VulkanRenderDevice::getActiveBackendName() const noexcept {
    return "Vulkan";
}

bool VulkanRenderDevice::isMDIUsable() const noexcept {
    if (!m_context) {
        return false;
    }
    return m_context->isMultiDrawIndirectEnabled() &&
           m_context->isDrawIndirectFirstInstanceEnabled();
}

void VulkanRenderDevice::renderFrame(RenderFrameParams &params) {
    const Camera &cullingCamera =
        params.cullingCamera ? *params.cullingCamera : params.activeCamera;
    renderFrameVulkan(params.chunkManager, params.activeCamera, cullingCamera, params.player,
                      params.sky.getSunDir());
}

void VulkanRenderDevice::renderFrameVulkan(ChunkManager &chunkManager, const Camera &activeCamera,
                                           const Camera &cullingCamera, const Player &player,
                                           const glm::vec3 &sunDirection) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };

    if (!ensureInitialized()) {
        return;
    }

    const int width = std::max(0, GameData::screenWidth);
    const int height = std::max(0, GameData::screenHeight);
    if (width == 0 || height == 0) {
        return;
    }

    ++m_frameCounter;
    collectRetiredChunkMeshes();
    collectRetiredRayTracingResources();

    const glm::ivec3 cullingChunk = chunkManager.worldToChunkPos(
        glm::ivec3(static_cast<int>(std::floor(cullingCamera.position.x)),
                   static_cast<int>(std::floor(cullingCamera.position.y)),
                   static_cast<int>(std::floor(cullingCamera.position.z))));

    const float sunLen2 = glm::dot(sunDirection, sunDirection);
    if (sunLen2 > 1.0e-8f && std::isfinite(sunLen2)) {
        m_giSunDirection = glm::normalize(sunDirection);
    } else {
        m_giSunDirection = glm::normalize(glm::vec3(0.25f, 0.85f, 0.42f));
    }

    const auto meshSyncStart = std::chrono::steady_clock::now();
    m_uploadContext.poll();
    syncChunkMeshes(chunkManager, cullingChunk);
    if (m_rtSceneDirty) {
        (void)rebuildRayTracingScene();
    }
    const auto meshSyncEnd = std::chrono::steady_clock::now();
    m_lastTimingSnapshot.cpuMeshSyncMs = measureMs(meshSyncStart, meshSyncEnd);
    updateGiProbes(chunkManager, cullingCamera.position);
    m_lastTimingSnapshot.cpuGiIntegrateMs = m_lastGiStats.cpuIntegrateMs;
    m_lastTimingSnapshot.gpuGiIntegrateMs = m_lastGiStats.gpuIntegrateMs;
    m_lastTimingSnapshot.giProbesUpdated = m_lastGiStats.probesUpdated;
    m_lastTimingSnapshot.giRaysCast = m_lastGiStats.raysCast;
    m_lastTimingSnapshot.giAverageIrradianceLuma = m_lastGiStats.averageIrradianceLuma;

    if (m_chunkMeshes.empty() && !m_warnedNoCpuChunkMeshes) {
        std::cerr << "[Vulkan] No chunk CPU meshes available yet. Waiting for chunk mesher.\n";
        m_warnedNoCpuChunkMeshes = true;
    }
    if (!m_chunkMeshes.empty()) {
        m_warnedNoCpuChunkMeshes = false;
    }

    bool resetRestirHistory = false;
    if (m_giHistoryAnchorValid) {
        const float cameraDelta = glm::length(cullingCamera.position - m_giHistoryAnchor);
        const float sunDelta =
            1.0f - glm::clamp(glm::dot(m_giSunDirection, m_prevGiHistorySunDir), -1.0f, 1.0f);
        resetRestirHistory = (cameraDelta > 6.0f) || (sunDelta > 0.08f);
    }
    m_giHistoryAnchor = cullingCamera.position;
    m_prevGiHistorySunDir = m_giSunDirection;
    m_giHistoryAnchorValid = true;

    const bool hardwareRtSupported =
        (m_context != nullptr) && m_context->isHardwareRayTracingSupported();
    const bool rtSceneReady = hardwareRtSupported && (m_activeGiSceneTlas != VK_NULL_HANDLE);
    const int runtimeTracingPreference = GameData::giTracingBackendPreference;
    bool preferHardwareRt = true;
    bool useEnvOverride = true;
    if (runtimeTracingPreference == 1) {
        preferHardwareRt = false;
        useEnvOverride = false;
    } else if (runtimeTracingPreference == 2) {
        preferHardwareRt = true;
        useEnvOverride = false;
    }
    if (useEnvOverride) {
        if (const char *env = std::getenv("VOXELOPS_GI_TRACING_BACKEND")) {
            const std::string mode = toLowerCopy(std::string(env));
            if (mode == "software" || mode == "dda" || mode == "compute") {
                preferHardwareRt = false;
            } else if (mode == "hardware" || mode == "rt" || mode == "rtcore" ||
                       mode == "rtcores") {
                preferHardwareRt = true;
            }
        }
    }
    const GiTracingBackend selectedGiTracingBackend = (preferHardwareRt && rtSceneReady)
                                                          ? GiTracingBackend::HardwareRt
                                                          : GiTracingBackend::SoftwareDda;
    if (preferHardwareRt && !rtSceneReady && !m_warnedHardwareRtUnavailable) {
        std::cerr << "[Vulkan][GI] Hardware RT backend requested but unavailable (support="
                  << (hardwareRtSupported ? "yes" : "no")
                  << ", scene=" << ((m_activeGiSceneTlas != VK_NULL_HANDLE) ? "ready" : "not-ready")
                  << "). "
                  << "Falling back to SoftwareDda.\n";
        m_warnedHardwareRtUnavailable = true;
    }
    if (!m_loggedGiTracingBackend || selectedGiTracingBackend != m_lastGiTracingBackend) {
        std::cout << "[Vulkan][GI] tracing backend="
                  << ((selectedGiTracingBackend == GiTracingBackend::HardwareRt) ? "HardwareRt"
                                                                                 : "SoftwareDda")
                  << " pref="
                  << ((runtimeTracingPreference == 1)   ? "SoftwareDda"
                      : (runtimeTracingPreference == 2) ? "HardwareRt"
                                                        : "Auto")
                  << " hwRtSupported=" << (hardwareRtSupported ? "true" : "false")
                  << " sceneTlas=" << ((m_activeGiSceneTlas != VK_NULL_HANDLE) ? "ready" : "none")
                  << "\n";
        m_loggedGiTracingBackend = true;
        m_lastGiTracingBackend = selectedGiTracingBackend;
    }

    m_frameData.clear();
    m_frameData.giLighting.hardwareRayTracingSupported = hardwareRtSupported;
    m_frameData.giLighting.tracingBackend = selectedGiTracingBackend;
    m_frameData.giLighting.sceneTlas = m_activeGiSceneTlas;
    m_frameData.giLighting.nrdDebugView =
        static_cast<uint32_t>(std::clamp(GameData::giNrdDebugView, 0, 5));
    m_lastTimingSnapshot.giHardwareRtSupported = hardwareRtSupported;
    m_lastTimingSnapshot.giRtSceneReady = rtSceneReady;
    m_lastTimingSnapshot.giTracingBackend = selectedGiTracingBackend;
    const float backendTraceDistanceCap =
        (selectedGiTracingBackend == GiTracingBackend::HardwareRt) ? 256.0f : 64.0f;
    if (m_giComputeState && m_giComputeState->ready) {
        const int worldMinX = WORLD_MIN_X * CHUNK_SIZE;
        const int worldMaxX = ((WORLD_MAX_X + 1) * CHUNK_SIZE) - 1;
        const int worldMinZ = WORLD_MIN_Z * CHUNK_SIZE;
        const int worldMaxZ = ((WORLD_MAX_Z + 1) * CHUNK_SIZE) - 1;

        const uint32_t lightingCascadeCount =
            std::min(static_cast<uint32_t>(GI_LIGHTING_MAX_CASCADES),
                     std::min(m_giConfig.cascadeCount, GiClipmapPlan::MAX_CASCADES));
        m_frameData.giLighting.cascadeCount = 0;
        m_frameData.giLighting.pathTracingEnabled = kEnablePathTracedGi;
        m_frameData.giLighting.pathTraceRaysPerPixel = kPathTraceRaysPerPixel;
        m_frameData.giLighting.pathTraceMaxBounces = kPathTraceMaxBounces;
        m_frameData.giLighting.pathTraceSkyIntensity = kPathTraceSkyIntensity;
        m_frameData.giLighting.baseDiffuse = kEnablePathTracedGi ? 0.12f : 0.50f;
        m_frameData.giLighting.giIntensity = 1.00f;
        m_frameData.giLighting.sunIntensity = 1.35f;
        m_frameData.giLighting.restirTemporalBlend = 0.86f;
        m_frameData.giLighting.restirSpatialReuse = 0.18f;
        m_frameData.giLighting.denoiseTemporalBlend = 0.92f;
        m_frameData.giLighting.denoiseSpatialWeight = 0.26f;
        m_frameData.giLighting.denoiseLumaPhi = 2.0f;
        m_frameData.giLighting.denoiseMomentBlend = 0.08f;
        m_frameData.giLighting.sunShadowMinVisibility = 0.00f;
        m_frameData.giLighting.sunShadowMaxDistance = backendTraceDistanceCap;
        m_frameData.giLighting.sunDirection = m_giSunDirection;
        m_frameData.giLighting.sunShadowsEnabled = false;
        m_frameData.giLighting.resetHistory = resetRestirHistory;
        m_frameData.giLighting.nrdDebugView =
            static_cast<uint32_t>(std::clamp(GameData::giNrdDebugView, 0, 5));
        m_frameData.giLighting.hardwareRayTracingSupported = hardwareRtSupported;
        m_frameData.giLighting.tracingBackend = selectedGiTracingBackend;
        m_frameData.giLighting.sceneTlas = m_activeGiSceneTlas;
        m_frameData.giLighting.traceMaterialBuffer = VK_NULL_HANDLE;
        m_frameData.giLighting.shadowWorldBoundsXy =
            glm::ivec4(worldMinX, worldMaxX, WORLD_MIN_Y, WORLD_MAX_Y);
        m_frameData.giLighting.shadowWorldBoundsZ = glm::ivec4(worldMinZ, worldMaxZ, 0, 0);

        int shadowCascadeIndex = -1;
        uint64_t shadowCascadeVolume = 0;
        for (uint32_t cascadeIndex = 0; cascadeIndex < lightingCascadeCount; ++cascadeIndex) {
            const auto &cascadeConfig = m_giConfig.cascades[cascadeIndex];
            const auto &cascadeState = m_giComputeState->cascades[cascadeIndex];
            if (cascadeState.probeBuffer == nullptr || cascadeConfig.probeCounts.x == 0 ||
                cascadeConfig.probeCounts.y == 0 || cascadeConfig.probeCounts.z == 0) {
                continue;
            }

            GiCascadeLightingData &lightingCascade = m_frameData.giLighting.cascades[cascadeIndex];
            lightingCascade.originSpacingBlocks =
                glm::ivec4(m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks,
                           static_cast<int>(std::max(1u, cascadeConfig.spacingBlocks)));
            lightingCascade.probeCounts = glm::uvec4(cascadeConfig.probeCounts, 0u);
            lightingCascade.probeBuffer = *cascadeState.probeBuffer;
            m_frameData.giLighting.cascadeCount = cascadeIndex + 1;

            if (cascadeState.occupancyBuffer != nullptr && cascadeState.occupancyWordCount > 0) {
                const uint64_t volume = static_cast<uint64_t>(cascadeState.occupancyDims.x) *
                                        static_cast<uint64_t>(cascadeState.occupancyDims.y) *
                                        static_cast<uint64_t>(cascadeState.occupancyDims.z);
                if (volume > shadowCascadeVolume) {
                    shadowCascadeVolume = volume;
                    shadowCascadeIndex = static_cast<int>(cascadeIndex);
                }
            }
        }
        m_frameData.giLighting.enabled = (m_frameData.giLighting.cascadeCount > 0);

        if (shadowCascadeIndex >= 0) {
            const uint32_t selectedIndex = static_cast<uint32_t>(shadowCascadeIndex);
            const auto &cascadeConfig = m_giConfig.cascades[selectedIndex];
            const auto &cascadeState = m_giComputeState->cascades[selectedIndex];
            m_frameData.giLighting.shadowOccupancyBuffer = (cascadeState.occupancyBuffer != nullptr)
                                                               ? *cascadeState.occupancyBuffer
                                                               : VK_NULL_HANDLE;
            m_frameData.giLighting.shadowOccupancyMinBlocks = cascadeState.occupancyMinBlocks;
            m_frameData.giLighting.shadowOccupancyDims = cascadeState.occupancyDims;
            m_frameData.giLighting.shadowOccupancyWordCount = cascadeState.occupancyWordCount;
            m_frameData.giLighting.traceMaterialBuffer = (cascadeState.materialBuffer != nullptr)
                                                             ? *cascadeState.materialBuffer
                                                             : VK_NULL_HANDLE;

            const uint32_t spacing = std::max(1u, cascadeConfig.spacingBlocks);
            const uint32_t maxProbeAxis =
                std::max({cascadeConfig.probeCounts.x, cascadeConfig.probeCounts.y,
                          cascadeConfig.probeCounts.z});
            const float traceDistance = std::max(64.0f, static_cast<float>(maxProbeAxis * spacing));
            m_frameData.giLighting.sunShadowMaxDistance =
                std::min(traceDistance, backendTraceDistanceCap);

            const bool traceSceneReady =
                (m_frameData.giLighting.shadowOccupancyBuffer != VK_NULL_HANDLE) &&
                (m_frameData.giLighting.traceMaterialBuffer != VK_NULL_HANDLE);
            if (traceSceneReady) {
                m_lastTraceSceneValid = true;
                m_lastTraceOccupancyBuffer = m_frameData.giLighting.shadowOccupancyBuffer;
                m_lastTraceMaterialBuffer = m_frameData.giLighting.traceMaterialBuffer;
                m_lastTraceOccupancyMinBlocks = m_frameData.giLighting.shadowOccupancyMinBlocks;
                m_lastTraceOccupancyDims = m_frameData.giLighting.shadowOccupancyDims;
                m_lastTraceOccupancyWordCount = m_frameData.giLighting.shadowOccupancyWordCount;
                m_lastTraceSunShadowMaxDistance = m_frameData.giLighting.sunShadowMaxDistance;
            } else if (m_lastTraceSceneValid) {
                m_frameData.giLighting.shadowOccupancyBuffer = m_lastTraceOccupancyBuffer;
                m_frameData.giLighting.traceMaterialBuffer = m_lastTraceMaterialBuffer;
                m_frameData.giLighting.shadowOccupancyMinBlocks = m_lastTraceOccupancyMinBlocks;
                m_frameData.giLighting.shadowOccupancyDims = m_lastTraceOccupancyDims;
                m_frameData.giLighting.shadowOccupancyWordCount = m_lastTraceOccupancyWordCount;
                m_frameData.giLighting.sunShadowMaxDistance = m_lastTraceSunShadowMaxDistance;
            } else {
                m_frameData.giLighting.pathTracingEnabled = false;
                m_frameData.giLighting.resetHistory = true;
            }
            m_frameData.giLighting.sunShadowsEnabled =
                (!m_frameData.giLighting.pathTracingEnabled) &&
                (m_frameData.giLighting.shadowOccupancyBuffer != VK_NULL_HANDLE);
        } else {
            if (m_lastTraceSceneValid) {
                m_frameData.giLighting.shadowOccupancyBuffer = m_lastTraceOccupancyBuffer;
                m_frameData.giLighting.traceMaterialBuffer = m_lastTraceMaterialBuffer;
                m_frameData.giLighting.shadowOccupancyMinBlocks = m_lastTraceOccupancyMinBlocks;
                m_frameData.giLighting.shadowOccupancyDims = m_lastTraceOccupancyDims;
                m_frameData.giLighting.shadowOccupancyWordCount = m_lastTraceOccupancyWordCount;
                m_frameData.giLighting.sunShadowMaxDistance = m_lastTraceSunShadowMaxDistance;
            } else {
                m_frameData.giLighting.pathTracingEnabled = false;
                m_frameData.giLighting.resetHistory = true;
            }
        }
    }

    const bool currentGiStable = m_frameData.giLighting.pathTracingEnabled &&
                                 (m_frameData.giLighting.shadowOccupancyBuffer != VK_NULL_HANDLE) &&
                                 (m_frameData.giLighting.traceMaterialBuffer != VK_NULL_HANDLE) &&
                                 (m_frameData.giLighting.shadowOccupancyDims.x > 0u) &&
                                 (m_frameData.giLighting.shadowOccupancyDims.y > 0u) &&
                                 (m_frameData.giLighting.shadowOccupancyDims.z > 0u);
    if (currentGiStable) {
        m_lastGiLighting = m_frameData.giLighting;
        m_lastGiLightingValid = true;
    } else if (m_lastGiLightingValid) {
        GiLightingData fallbackLighting = m_lastGiLighting;
        fallbackLighting.sunDirection = m_giSunDirection;
        fallbackLighting.pathTracingEnabled = kEnablePathTracedGi;
        fallbackLighting.sunShadowsEnabled = false;
        fallbackLighting.resetHistory = true;
        fallbackLighting.hardwareRayTracingSupported = hardwareRtSupported;
        fallbackLighting.tracingBackend = selectedGiTracingBackend;
        fallbackLighting.sceneTlas = m_activeGiSceneTlas;
        m_frameData.giLighting = fallbackLighting;
    }

    m_frameData.modelMatrices.reserve(m_chunkMeshes.size());
    m_frameData.indirectCommands.reserve(m_chunkMeshes.size());
    m_frameData.indirectBatches.reserve(m_chunkMeshes.size());

    glm::mat4 view = activeCamera.getViewMatrix();
    glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(GameData::FOV), static_cast<float>(width) / static_cast<float>(height), 0.1f,
        1000.0f);
    projection[1][1] *= -1.0f;
    const glm::mat4 viewProjection = projection * view;
    Frustum frustum;
    const glm::mat4 cullingViewProjection = projection * cullingCamera.getViewMatrix();
    frustum.extractPlanes(cullingViewProjection, true);

    const int maxRenderDistance = std::max(2, static_cast<int>(player.renderDistance));
    const int64_t radius2 =
        static_cast<int64_t>(maxRenderDistance) * static_cast<int64_t>(maxRenderDistance);
    const auto frameBuildStart = std::chrono::steady_clock::now();

    for (const auto &[chunkPos, cached] : m_chunkMeshes) {
        if (cached.mesh.getIndexCount() == 0) {
            continue;
        }

        const glm::ivec3 d = chunkPos - cullingChunk;
        const int64_t dist2 = static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
                              static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
        if (dist2 > radius2) {
            continue;
        }

        const glm::vec3 chunkMin = glm::vec3(chunkPos * CHUNK_SIZE);
        const glm::vec3 chunkMax = chunkMin + glm::vec3(CHUNK_SIZE);
        if (!frustum.isBoxVisible(chunkMin, chunkMax)) {
            continue;
        }

        const glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMin);
        const uint32_t modelIndex = static_cast<uint32_t>(m_frameData.modelMatrices.size());
        m_frameData.modelMatrices.push_back(model);

        const uint32_t commandIndex = static_cast<uint32_t>(m_frameData.indirectCommands.size());
        IndexedIndirectCommand command{};
        command.indexCount = cached.mesh.getIndexCount();
        command.instanceCount = 1;
        command.firstIndex = 0;
        command.vertexOffset = 0;
        command.firstInstance = modelIndex;
        m_frameData.indirectCommands.push_back(command);

        RenderIndirectBatch batch{};
        batch.mesh = &cached.mesh;
        batch.texture = &m_atlasTexture;
        batch.firstCommand = commandIndex;
        batch.commandCount = 1;
        m_frameData.indirectBatches.push_back(batch);
    }

    if (ensureRemotePlayerAssetsLoaded() && m_remotePlayerModel &&
        m_remotePlayerModel->hasLocalBounds()) {
        constexpr float kLocalGhostRejectDistance = 2.0f;
        const float localGhostRejectDistanceSq =
            kLocalGhostRejectDistance * kLocalGhostRejectDistance;

        const glm::vec3 localMin = m_remotePlayerModel->getLocalMinBounds();
        const glm::vec3 localMax = m_remotePlayerModel->getLocalMaxBounds();
        const glm::vec3 modelSize = localMax - localMin;
        const float targetHeight =
            std::max(Shared::PlayerData::GetMovementSettings().collisionHeight, 0.01f);
        const float uniformFitToCollision = targetHeight / std::max(modelSize.y, 1.0e-4f);
        const float modelMinY = localMin.y;

        m_frameData.objects.reserve(m_frameData.objects.size() + player.connectedPlayers.size());
        for (const auto &[_, state] : player.connectedPlayers) {
            const glm::vec3 toLocal = state.position - player.getPosition();
            const float localDistSq = glm::dot(toLocal, toLocal);
            if (!std::isfinite(localDistSq) || localDistSq < localGhostRejectDistanceSq) {
                continue;
            }

            const glm::vec3 scaled = state.scale * uniformFitToCollision;
            const glm::vec3 anchoredPos =
                state.position + glm::vec3(0.0f, -modelMinY * scaled.y, 0.0f);
            const glm::quat safeRotation = (glm::dot(state.rotation, state.rotation) > 1.0e-10f)
                                               ? glm::normalize(state.rotation)
                                               : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

            RenderObject object{};
            object.model = m_remotePlayerModel.get();
            object.meshTextures = &m_remotePlayerTextureViews;
            object.transform = glm::translate(glm::mat4(1.0f), anchoredPos) *
                               glm::toMat4(safeRotation) * glm::scale(glm::mat4(1.0f), scaled);
            m_frameData.objects.push_back(object);
        }
    }
    const auto frameBuildEnd = std::chrono::steady_clock::now();
    m_lastTimingSnapshot.cpuFrameBuildMs = measureMs(frameBuildStart, frameBuildEnd);

    try {
        m_renderer->renderFrame(static_cast<uint32_t>(width), static_cast<uint32_t>(height), view,
                                projection, viewProjection, m_frameData);
        const VulkanRenderer::FrameTimingStats &rendererStats =
            m_renderer->getLastFrameTimingStats();
        m_lastTimingSnapshot.gpuValid = rendererStats.gpuValid;
        m_lastTimingSnapshot.gpuFrameMs = rendererStats.gpuFrameMs;
        m_lastTimingSnapshot.gpuChunkPassMs = rendererStats.gpuChunkPassMs;
        m_lastTimingSnapshot.gpuModelPassMs = rendererStats.gpuModelPassMs;
        m_lastTimingSnapshot.gpuUiPassMs = rendererStats.gpuUiPassMs;
        m_lastTimingSnapshot.cpuCommandRecordMs = rendererStats.cpuCommandRecordMs;
        m_lastTimingSnapshot.cpuChunkPassMs = rendererStats.cpuChunkPassMs;
        m_lastTimingSnapshot.cpuModelPassMs = rendererStats.cpuModelPassMs;
        m_lastTimingSnapshot.cpuUiPassMs = rendererStats.cpuUiPassMs;
        m_lastTimingSnapshot.nrdBootstrapActive = m_renderer->isNrdBootstrapActive();
        m_lastTimingSnapshot.nrdBootstrapDispatchCount = m_renderer->getNrdBootstrapDispatchCount();
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan] renderFrame failed: " << e.what() << "\n";
    }
}

void VulkanRenderDevice::shutdown() {
    if (!m_initialized && !m_context && !m_renderer) {
        return;
    }

    try {
        if (m_context) {
            m_context->getDevice().waitIdle();
        }
    } catch (...) {
    }

    cleanupChunkMeshes();
    resetRayTracingScene();
    cleanupRemotePlayerAssets();
    resetGiClipmaps();
    m_atlasTexture.cleanup();
    m_atlasTextureLoaded = false;

    m_uploadContext.cleanup();

    if (m_renderer) {
        m_renderer->cleanup();
        m_renderer.reset();
    }
    if (m_context) {
        m_context->cleanup();
        m_context.reset();
    }

    m_window = nullptr;
    m_initialized = false;
}

std::string_view VulkanRenderDevice::getApiName() const noexcept {
    return "Vulkan";
}

bool VulkanRenderDevice::ensureInitialized() {
    if (m_initialized && m_context && m_renderer) {
        return true;
    }
    if (!m_warnedUninitializedRender) {
        std::cerr << "[Vulkan] render skipped: device not initialized.\n";
        m_warnedUninitializedRender = true;
    }
    return false;
}

bool VulkanRenderDevice::ensureAtlasTextureLoaded() {
    if (m_atlasTextureLoaded) {
        return true;
    }
    if (!m_context) {
        return false;
    }

    const std::string atlasPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("assets/textures/textureAtlas.png")
            .generic_string();
    m_atlasTexture.initFromAtlasFileAsArray(
        m_context->getDevice(), m_context->getPhysicalDevice(), m_uploadContext, atlasPath,
        static_cast<uint32_t>(TEXTURE_ATLAS_SIZE), m_context->isSamplerAnisotropyEnabled(),
        m_context->getMaxSamplerAnisotropy());
    m_atlasTextureLoaded = true;
    return true;
}

bool VulkanRenderDevice::ensureRemotePlayerAssetsLoaded() {
    if (m_remotePlayerAssetsLoaded) {
        return true;
    }
    if (!m_context) {
        return false;
    }

    try {
        cleanupRemotePlayerAssets();

        const std::string modelPath =
            Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();
        m_remotePlayerModel = std::make_unique<VkModel>();
        m_remotePlayerModel->loadModel(modelPath);
        m_remotePlayerModel->initGpuResources(m_context->getDevice(),
                                              m_context->getPhysicalDevice(), m_uploadContext);

        const auto &meshTexturePaths = m_remotePlayerModel->getMeshTexturePaths();
        m_remotePlayerTextures.clear();
        m_remotePlayerTextures.reserve(meshTexturePaths.size());
        for (const std::string &texturePath : meshTexturePaths) {
            VkTexture texture{};
            texture.initFromFile(m_context->getDevice(), m_context->getPhysicalDevice(),
                                 m_uploadContext, texturePath,
                                 m_context->isSamplerAnisotropyEnabled(),
                                 m_context->getMaxSamplerAnisotropy());
            m_remotePlayerTextures.emplace_back(std::move(texture));
        }

        m_remotePlayerTextureViews.clear();
        m_remotePlayerTextureViews.reserve(m_remotePlayerTextures.size());
        for (const VkTexture &texture : m_remotePlayerTextures) {
            m_remotePlayerTextureViews.push_back(&texture);
        }

        m_uploadContext.waitIdle();
        m_remotePlayerAssetsLoaded = true;
        m_warnedRemotePlayerAssets = false;
        return true;
    } catch (const std::exception &e) {
        if (!m_warnedRemotePlayerAssets) {
            std::cerr << "[Vulkan] Failed to load remote player model assets: " << e.what() << "\n";
            m_warnedRemotePlayerAssets = true;
        }
        cleanupRemotePlayerAssets();
        return false;
    }
}

void VulkanRenderDevice::initGiClipmaps() {
    m_giConfig = GiClipmapPlan::makeDefaultConfig();

    // Keep probe update budget bounded while compute path is still host-fed.
    if (m_giConfig.cascadeCount > 0) {
        m_giConfig.cascades[0].schedule.maxProbeUpdatesPerTick = 24;
        m_giConfig.cascades[0].schedule.raysPerProbe = 16;
    }
    if (m_giConfig.cascadeCount > 1) {
        m_giConfig.cascades[1].schedule.maxProbeUpdatesPerTick = 16;
        m_giConfig.cascades[1].schedule.raysPerProbe = 12;
    }
    if (m_giConfig.cascadeCount > 2) {
        m_giConfig.cascades[2].schedule.maxProbeUpdatesPerTick = 8;
        m_giConfig.cascades[2].schedule.raysPerProbe = 8;
    }

    for (uint32_t cascadeIndex = 0; cascadeIndex < GiClipmapPlan::MAX_CASCADES; ++cascadeIndex) {
        m_giCascadeRuntime[cascadeIndex] = GiClipmapPlan::GiCascadeRuntimeState{};
    }

    const uint32_t cascadeCount = std::min(m_giConfig.cascadeCount, GiClipmapPlan::MAX_CASCADES);
    m_lastGiStats = GiRuntimeStats{};
    if (!initGiComputeResources()) {
        std::cerr << "[Vulkan][GI] Compute init failed, GI probe integration disabled.\n";
    }
    std::cout << "[Vulkan][GI] Clipmap init cascades=" << cascadeCount
              << " | worstRays/frame=" << GiClipmapPlan::estimateWorstCaseRaysPerFrame(m_giConfig)
              << " | avgRays/frame=" << GiClipmapPlan::estimateAverageRaysPerFrame(m_giConfig)
              << "\n";
}

void VulkanRenderDevice::resetGiClipmaps() {
    for (uint32_t cascadeIndex = 0; cascadeIndex < GiClipmapPlan::MAX_CASCADES; ++cascadeIndex) {
        m_giCascadeRuntime[cascadeIndex] = GiClipmapPlan::GiCascadeRuntimeState{};
    }
    resetGiComputeResources();
    m_lastTraceSceneValid = false;
    m_lastTraceOccupancyBuffer = VK_NULL_HANDLE;
    m_lastTraceMaterialBuffer = VK_NULL_HANDLE;
    m_lastTraceOccupancyMinBlocks = glm::ivec3(0);
    m_lastTraceOccupancyDims = glm::uvec3(0u);
    m_lastTraceOccupancyWordCount = 0;
    m_lastTraceSunShadowMaxDistance = 64.0f;
    m_lastGiLightingValid = false;
    m_lastGiLighting = GiLightingData{};
    m_giHistoryAnchorValid = false;
    m_giHistoryAnchor = glm::vec3(0.0f);
    m_prevGiHistorySunDir = glm::vec3(0.25f, 0.85f, 0.42f);
    m_lastGiStats = GiRuntimeStats{};
    m_warnedHardwareRtUnavailable = false;
    m_loggedGiTracingBackend = false;
    m_lastGiTracingBackend = GiTracingBackend::SoftwareDda;
}

bool VulkanRenderDevice::initGiComputeResources() {
    resetGiComputeResources();
    if (!m_context) {
        return false;
    }

    const uint32_t cascadeCount = std::min(m_giConfig.cascadeCount, GiClipmapPlan::MAX_CASCADES);
    if (cascadeCount == 0) {
        return false;
    }

    auto state = std::make_unique<GiComputeState>();
    const vk::raii::Device &device = m_context->getDevice();

    try {
        vk::DescriptorSetLayoutBinding occupancyBinding{};
        occupancyBinding.binding = 0;
        occupancyBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
        occupancyBinding.descriptorCount = 1;
        occupancyBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutBinding materialBinding{};
        materialBinding.binding = 1;
        materialBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
        materialBinding.descriptorCount = 1;
        materialBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutBinding probeBinding{};
        probeBinding.binding = 2;
        probeBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
        probeBinding.descriptorCount = 1;
        probeBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutBinding statsBinding{};
        statsBinding.binding = 3;
        statsBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
        statsBinding.descriptorCount = 1;
        statsBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;

        const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {
            occupancyBinding, materialBinding, probeBinding, statsBinding};
        vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
        descriptorSetLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorSetLayoutInfo.pBindings = bindings.data();
        state->descriptorSetLayout = vk::raii::DescriptorSetLayout(device, descriptorSetLayoutInfo);

        std::array<vk::DescriptorPoolSize, 4> poolSizes{};
        poolSizes[0].type = vk::DescriptorType::eStorageBuffer;
        poolSizes[0].descriptorCount = cascadeCount;
        poolSizes[1].type = vk::DescriptorType::eStorageBuffer;
        poolSizes[1].descriptorCount = cascadeCount;
        poolSizes[2].type = vk::DescriptorType::eStorageBuffer;
        poolSizes[2].descriptorCount = cascadeCount;
        poolSizes[3].type = vk::DescriptorType::eStorageBuffer;
        poolSizes[3].descriptorCount = cascadeCount;

        vk::DescriptorPoolCreateInfo descriptorPoolInfo{};
        descriptorPoolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        descriptorPoolInfo.maxSets = cascadeCount;
        descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        descriptorPoolInfo.pPoolSizes = poolSizes.data();
        state->descriptorPool = vk::raii::DescriptorPool(device, descriptorPoolInfo);

        std::vector<vk::DescriptorSetLayout> layouts(cascadeCount, *state->descriptorSetLayout);
        vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo{};
        descriptorSetAllocateInfo.descriptorPool = *state->descriptorPool;
        descriptorSetAllocateInfo.descriptorSetCount = cascadeCount;
        descriptorSetAllocateInfo.pSetLayouts = layouts.data();
        state->descriptorSets = device.allocateDescriptorSets(descriptorSetAllocateInfo);

        std::string shaderDir;
#ifdef SHADER_DIR
        shaderDir = SHADER_DIR;
#endif
        if (!shaderDir.empty() && shaderDir.back() != '/' && shaderDir.back() != '\\') {
            shaderDir.push_back('/');
        }
        vk::raii::ShaderModule computeShader =
            loadShaderModule(device, shaderDir + "gi_probe_integrate.comp.spv");

        vk::PipelineShaderStageCreateInfo stageInfo{};
        stageInfo.stage = vk::ShaderStageFlagBits::eCompute;
        stageInfo.module = *computeShader;
        stageInfo.pName = "main";

        vk::PushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
        pushConstantRange.offset = 0;
        pushConstantRange.size = static_cast<uint32_t>(sizeof(GiComputePushConstants));

        const vk::DescriptorSetLayout descriptorSetLayout = *state->descriptorSetLayout;
        vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        state->pipelineLayout = vk::raii::PipelineLayout(device, pipelineLayoutInfo);

        vk::ComputePipelineCreateInfo computePipelineInfo{};
        computePipelineInfo.stage = stageInfo;
        computePipelineInfo.layout = *state->pipelineLayout;
        state->pipeline = vk::raii::Pipeline(device, nullptr, computePipelineInfo);

        vk::CommandPoolCreateInfo commandPoolInfo{};
        commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        commandPoolInfo.queueFamilyIndex = m_context->getGraphicsQueueFamily();
        state->commandPool = vk::raii::CommandPool(device, commandPoolInfo);

        vk::CommandBufferAllocateInfo commandBufferAllocateInfo{};
        commandBufferAllocateInfo.commandPool = *state->commandPool;
        commandBufferAllocateInfo.level = vk::CommandBufferLevel::ePrimary;
        commandBufferAllocateInfo.commandBufferCount = 1;
        auto commandBuffers = device.allocateCommandBuffers(commandBufferAllocateInfo);
        state->commandBuffer = std::move(commandBuffers.front());

        vk::FenceCreateInfo fenceInfo{};
        state->fence = vk::raii::Fence(device, fenceInfo);

        state->timestampEnabled = m_context->areTimestampQueriesSupported() &&
                                  m_context->getTimestampPeriodNanoseconds() > 0.0f;
        state->timestampPeriodNs =
            state->timestampEnabled ? m_context->getTimestampPeriodNanoseconds() : 0.0f;
        if (state->timestampEnabled) {
            vk::QueryPoolCreateInfo queryPoolInfo{};
            queryPoolInfo.queryType = vk::QueryType::eTimestamp;
            queryPoolInfo.queryCount = 2;
            state->queryPool = vk::raii::QueryPool(device, queryPoolInfo);
        }

        state->ready = true;
        m_giComputeState = std::move(state);
        return true;
    } catch (const std::exception &e) {
        state->reset();
        std::cerr << "[Vulkan][GI] Failed to initialize compute resources: " << e.what() << "\n";
        return false;
    }
}

void VulkanRenderDevice::resetGiComputeResources() {
    if (!m_giComputeState) {
        return;
    }
    m_giComputeState->reset();
    m_giComputeState.reset();
}

bool VulkanRenderDevice::dispatchGiProbeCompute(ChunkManager &chunkManager,
                                                const GiClipmapPlan::GiFrameBudget &frameBudget,
                                                uint32_t cascadeCount) {
    if (!m_context || !m_giComputeState || !m_giComputeState->ready) {
        return false;
    }

    GiComputeState &state = *m_giComputeState;
    const vk::raii::Device &device = m_context->getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = m_context->getPhysicalDevice();
    const vk::raii::Queue &queue = m_context->getGraphicsQueue();

    const int worldMinX = WORLD_MIN_X * CHUNK_SIZE;
    const int worldMaxX = ((WORLD_MAX_X + 1) * CHUNK_SIZE) - 1;
    const int worldMinZ = WORLD_MIN_Z * CHUNK_SIZE;
    const int worldMaxZ = ((WORLD_MAX_Z + 1) * CHUNK_SIZE) - 1;

    bool hasWork = false;
    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        const GiClipmapPlan::GiCascadeConfig &cascade = m_giConfig.cascades[cascadeIndex];
        const GiClipmapPlan::GiCascadeFrameBudget &budget = frameBudget.cascades[cascadeIndex];
        if (budget.probesUpdated == 0) {
            continue;
        }

        const uint32_t totalProbes = GiClipmapPlan::probeCount(cascade);
        if (totalProbes == 0) {
            continue;
        }

        const uint32_t spacing = std::max(1u, cascade.spacingBlocks);
        const glm::ivec3 probeExtentBlocks =
            glm::ivec3(cascade.probeCounts) * static_cast<int>(spacing);
        const int paddingBlocks = static_cast<int>(spacing * kGiOccupancyPaddingMultiplier);
        const glm::ivec3 occupancyMin =
            m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks - glm::ivec3(paddingBlocks);
        const glm::uvec3 occupancyDims =
            glm::uvec3(std::max(1, probeExtentBlocks.x + (paddingBlocks * 2)),
                       std::max(1, probeExtentBlocks.y + (paddingBlocks * 2)),
                       std::max(1, probeExtentBlocks.z + (paddingBlocks * 2)));

        const uint64_t occupancyVoxelCount = static_cast<uint64_t>(occupancyDims.x) *
                                             static_cast<uint64_t>(occupancyDims.y) *
                                             static_cast<uint64_t>(occupancyDims.z);
        if (occupancyVoxelCount == 0) {
            continue;
        }
        const uint64_t occupancyWordCount64 = (occupancyVoxelCount + 31ull) / 32ull;
        if (occupancyWordCount64 > static_cast<uint64_t>(UINT32_MAX)) {
            std::cerr << "[Vulkan][GI] Occupancy buffer too large for cascade " << cascadeIndex
                      << "\n";
            continue;
        }
        const uint32_t occupancyWordCount = static_cast<uint32_t>(occupancyWordCount64);

        GiComputeState::CascadeResources &resources = state.cascades[cascadeIndex];
        const bool needsRealloc =
            resources.occupancyBuffer == nullptr || resources.materialBuffer == nullptr ||
            resources.probeBuffer == nullptr || resources.statsBuffer == nullptr ||
            resources.occupancyDims != occupancyDims ||
            resources.occupancyWordCount != occupancyWordCount ||
            resources.probeCount != totalProbes;

        if (needsRealloc) {
            resources.reset();
            resources.occupancyDims = occupancyDims;
            resources.occupancyWordCount = occupancyWordCount;
            resources.probeCount = totalProbes;
            resources.occupancyWords.assign(occupancyWordCount, 0u);
            resources.materialIds.assign(static_cast<size_t>(occupancyVoxelCount), 0u);

            VulkanUtils::createBuffer(device, physicalDevice,
                                      static_cast<vk::DeviceSize>(resources.occupancyWordCount) *
                                          sizeof(uint32_t),
                                      vk::BufferUsageFlagBits::eStorageBuffer,
                                      vk::MemoryPropertyFlagBits::eHostVisible |
                                          vk::MemoryPropertyFlagBits::eHostCoherent,
                                      resources.occupancyBuffer, resources.occupancyMemory);
            resources.occupancyMapped = resources.occupancyMemory.mapMemory(0, VK_WHOLE_SIZE);

            VulkanUtils::createBuffer(device, physicalDevice,
                                      static_cast<vk::DeviceSize>(occupancyVoxelCount) *
                                          sizeof(uint32_t),
                                      vk::BufferUsageFlagBits::eStorageBuffer,
                                      vk::MemoryPropertyFlagBits::eHostVisible |
                                          vk::MemoryPropertyFlagBits::eHostCoherent,
                                      resources.materialBuffer, resources.materialMemory);
            resources.materialMapped = resources.materialMemory.mapMemory(0, VK_WHOLE_SIZE);

            VulkanUtils::createBuffer(device, physicalDevice,
                                      static_cast<vk::DeviceSize>(resources.probeCount) *
                                          sizeof(GpuProbeSample),
                                      vk::BufferUsageFlagBits::eStorageBuffer,
                                      vk::MemoryPropertyFlagBits::eHostVisible |
                                          vk::MemoryPropertyFlagBits::eHostCoherent,
                                      resources.probeBuffer, resources.probeMemory);
            resources.probeMapped = resources.probeMemory.mapMemory(0, VK_WHOLE_SIZE);
            std::vector<GpuProbeSample> initialSamples(resources.probeCount);
            std::memcpy(resources.probeMapped, initialSamples.data(),
                        initialSamples.size() * sizeof(GpuProbeSample));

            VulkanUtils::createBuffer(device, physicalDevice, sizeof(GpuProbeStats),
                                      vk::BufferUsageFlagBits::eStorageBuffer,
                                      vk::MemoryPropertyFlagBits::eHostVisible |
                                          vk::MemoryPropertyFlagBits::eHostCoherent,
                                      resources.statsBuffer, resources.statsMemory);
            resources.statsMapped = resources.statsMemory.mapMemory(0, VK_WHOLE_SIZE);

            vk::DescriptorBufferInfo occupancyInfo{};
            occupancyInfo.buffer = *resources.occupancyBuffer;
            occupancyInfo.offset = 0;
            occupancyInfo.range =
                static_cast<vk::DeviceSize>(resources.occupancyWordCount) * sizeof(uint32_t);

            vk::DescriptorBufferInfo materialInfo{};
            materialInfo.buffer = *resources.materialBuffer;
            materialInfo.offset = 0;
            materialInfo.range =
                static_cast<vk::DeviceSize>(occupancyVoxelCount) * sizeof(uint32_t);

            vk::DescriptorBufferInfo probeInfo{};
            probeInfo.buffer = *resources.probeBuffer;
            probeInfo.offset = 0;
            probeInfo.range =
                static_cast<vk::DeviceSize>(resources.probeCount) * sizeof(GpuProbeSample);

            vk::DescriptorBufferInfo statsInfo{};
            statsInfo.buffer = *resources.statsBuffer;
            statsInfo.offset = 0;
            statsInfo.range = sizeof(GpuProbeStats);

            std::array<vk::WriteDescriptorSet, 4> writes{};
            writes[0].dstSet = *state.descriptorSets[cascadeIndex];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &occupancyInfo;

            writes[1].dstSet = *state.descriptorSets[cascadeIndex];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo = &materialInfo;

            writes[2].dstSet = *state.descriptorSets[cascadeIndex];
            writes[2].dstBinding = 2;
            writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
            writes[2].descriptorCount = 1;
            writes[2].pBufferInfo = &probeInfo;

            writes[3].dstSet = *state.descriptorSets[cascadeIndex];
            writes[3].dstBinding = 3;
            writes[3].descriptorType = vk::DescriptorType::eStorageBuffer;
            writes[3].descriptorCount = 1;
            writes[3].pBufferInfo = &statsInfo;
            device.updateDescriptorSets(writes, {});
        }

        const glm::ivec3 snappedOrigin = m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks;
        const bool originChanged = resources.occupancyAnchorSnappedOrigin != snappedOrigin;
        const uint32_t occupancyRefreshFrames =
            std::max(1u, cascade.schedule.updateEveryNFrames * 8u);
        const bool periodicRefresh = resources.occupancyLastBuildFrame == 0 ||
                                     (m_frameCounter - resources.occupancyLastBuildFrame) >=
                                         static_cast<uint64_t>(occupancyRefreshFrames);
        const bool shouldRebuildOccupancy = needsRealloc || originChanged || periodicRefresh;

        if (shouldRebuildOccupancy) {
            resources.occupancyMinBlocks = occupancyMin;
            std::fill(resources.occupancyWords.begin(), resources.occupancyWords.end(), 0u);
            std::fill(resources.materialIds.begin(), resources.materialIds.end(), 0u);

            const uint32_t dimX = resources.occupancyDims.x;
            const uint32_t dimY = resources.occupancyDims.y;
            const uint32_t dimZ = resources.occupancyDims.z;
            for (uint32_t z = 0; z < dimZ; ++z) {
                const int worldZ = occupancyMin.z + static_cast<int>(z);
                const bool zOut = worldZ < worldMinZ || worldZ > worldMaxZ;
                for (uint32_t y = 0; y < dimY; ++y) {
                    const int worldY = occupancyMin.y + static_cast<int>(y);
                    const bool yBelowWorld = worldY < WORLD_MIN_Y;
                    const bool yAboveWorld = worldY > WORLD_MAX_Y;
                    for (uint32_t x = 0; x < dimX; ++x) {
                        const int worldX = occupancyMin.x + static_cast<int>(x);
                        const bool xOut = worldX < worldMinX || worldX > worldMaxX;

                        BlockID blockId = BlockID::Air;
                        bool solid = false;
                        if (yBelowWorld) {
                            solid = true;
                            blockId = BlockID::Bedrock;
                        } else if (!yAboveWorld && !xOut && !zOut) {
                            blockId = chunkManager.getBlockGlobal(worldX, worldY, worldZ);
                            solid = isSolidBlock(blockId);
                        }

                        const uint32_t linear = x + dimX * (y + (dimY * z));
                        resources.materialIds[linear] = solid ? static_cast<uint32_t>(blockId) : 0u;
                        if (!solid) {
                            continue;
                        }
                        setOccupancyBit(resources.occupancyWords, linear);
                    }
                }
            }

            std::memcpy(resources.occupancyMapped, resources.occupancyWords.data(),
                        resources.occupancyWords.size() * sizeof(uint32_t));
            std::memcpy(resources.materialMapped, resources.materialIds.data(),
                        resources.materialIds.size() * sizeof(uint32_t));
            resources.occupancyAnchorSnappedOrigin = snappedOrigin;
            resources.occupancyLastBuildFrame = m_frameCounter;
        }

        const GpuProbeStats resetStats{};
        std::memcpy(resources.statsMapped, &resetStats, sizeof(resetStats));
        hasWork = true;
    }

    if (!hasWork) {
        return true;
    }

    state.commandBuffer.reset();
    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    state.commandBuffer.begin(beginInfo);

    if (state.timestampEnabled && state.queryPool != nullptr) {
        state.commandBuffer.resetQueryPool(*state.queryPool, 0, 2);
        state.commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *state.queryPool,
                                           0);
    }

    state.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *state.pipeline);
    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        const GiClipmapPlan::GiCascadeConfig &cascade = m_giConfig.cascades[cascadeIndex];
        const GiClipmapPlan::GiCascadeFrameBudget &budget = frameBudget.cascades[cascadeIndex];
        if (budget.probesUpdated == 0) {
            continue;
        }

        const uint32_t totalProbes = GiClipmapPlan::probeCount(cascade);
        if (totalProbes == 0) {
            continue;
        }

        const GiComputeState::CascadeResources &resources = state.cascades[cascadeIndex];
        if (resources.occupancyBuffer == nullptr || resources.materialBuffer == nullptr ||
            resources.probeBuffer == nullptr || resources.statsBuffer == nullptr) {
            continue;
        }

        const uint32_t spacing = std::max(1u, cascade.spacingBlocks);
        const uint32_t maxProbeAxis =
            std::max({cascade.probeCounts.x, cascade.probeCounts.y, cascade.probeCounts.z});
        const float maxTraceDistance = std::max(48.0f, static_cast<float>(maxProbeAxis * spacing));

        GiComputePushConstants push{};
        push.firstProbe = budget.firstProbe;
        push.probesUpdated = budget.probesUpdated;
        push.totalProbes = totalProbes;
        push.raysPerProbe = std::max(1u, cascade.schedule.raysPerProbe);

        push.snappedOriginX = m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks.x;
        push.snappedOriginY = m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks.y;
        push.snappedOriginZ = m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks.z;
        push.spacingBlocks = spacing;

        push.probeCountX = cascade.probeCounts.x;
        push.probeCountY = cascade.probeCounts.y;
        push.probeCountZ = cascade.probeCounts.z;
        push.frameIndexLow = static_cast<uint32_t>(m_frameCounter & 0xffffffffull);

        push.occupancyMinX = resources.occupancyMinBlocks.x;
        push.occupancyMinY = resources.occupancyMinBlocks.y;
        push.occupancyMinZ = resources.occupancyMinBlocks.z;
        push.occupancyDimX = resources.occupancyDims.x;
        push.occupancyDimY = resources.occupancyDims.y;
        push.occupancyDimZ = resources.occupancyDims.z;
        push.occupancyWordCount = resources.occupancyWordCount;

        push.worldMinX = worldMinX;
        push.worldMaxX = worldMaxX;
        push.worldMinY = WORLD_MIN_Y;
        push.worldMaxY = WORLD_MAX_Y;
        push.worldMinZ = worldMinZ;
        push.worldMaxZ = worldMaxZ;
        push.maxTraceDistance = maxTraceDistance;
        push.temporalBlend = kGiTemporalBlend;
        push.sunDirX = m_giSunDirection.x;
        push.sunDirY = m_giSunDirection.y;
        push.sunDirZ = m_giSunDirection.z;
        push.sunIntensity = m_giSunIntensity;

        const vk::DescriptorSet descriptorSet = *state.descriptorSets[cascadeIndex];
        state.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                               *state.pipelineLayout, 0, descriptorSet, {});
        state.commandBuffer.pushConstants<GiComputePushConstants>(
            *state.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, push);

        const uint32_t groupCount = (budget.probesUpdated + 63u) / 64u;
        if (groupCount > 0) {
            state.commandBuffer.dispatch(groupCount, 1, 1);
        }
    }

    if (state.timestampEnabled && state.queryPool != nullptr) {
        state.commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                           *state.queryPool, 1);
    }
    state.commandBuffer.end();

    std::array<vk::Fence, 1> fences = {*state.fence};
    (void)device.resetFences(fences);

    const vk::CommandBuffer rawCommandBuffer = *state.commandBuffer;
    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &rawCommandBuffer;
    queue.submit(submitInfo, *state.fence);
    (void)device.waitForFences(fences, vk::True, std::numeric_limits<uint64_t>::max());

    m_lastGiStats.gpuIntegrateMs = 0.0f;
    if (state.timestampEnabled && state.queryPool != nullptr) {
        std::array<uint64_t, 2> ticks{};
        const VkResult result = vkGetQueryPoolResults(
            static_cast<VkDevice>(*device), static_cast<VkQueryPool>(*state.queryPool), 0, 2,
            sizeof(ticks), ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS && ticks[1] >= ticks[0]) {
            const double tickToMs = static_cast<double>(state.timestampPeriodNs) * 1.0e-6;
            m_lastGiStats.gpuIntegrateMs =
                static_cast<float>(static_cast<double>(ticks[1] - ticks[0]) * tickToMs);
        }
    }

    uint64_t totalLumaFixed = 0;
    uint64_t totalLumaCount = 0;
    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        const GiClipmapPlan::GiCascadeFrameBudget &budget = frameBudget.cascades[cascadeIndex];
        if (budget.probesUpdated == 0) {
            continue;
        }
        const GiComputeState::CascadeResources &resources = state.cascades[cascadeIndex];
        if (resources.statsMapped == nullptr) {
            continue;
        }
        const auto *stats = reinterpret_cast<const GpuProbeStats *>(resources.statsMapped);
        totalLumaFixed += stats->lumaSumFixed;
        totalLumaCount += stats->lumaCount;
    }
    if (totalLumaCount > 0) {
        m_lastGiStats.averageIrradianceLuma = static_cast<float>(
            static_cast<double>(totalLumaFixed) /
            (static_cast<double>(kGiLumaFixedScale) * static_cast<double>(totalLumaCount)));
    }

    return true;
}

void VulkanRenderDevice::updateGiProbes(ChunkManager &chunkManager,
                                        const glm::vec3 &probeAnchorPosition) {
    const auto measureMs = [](auto start, auto end) -> float {
        return static_cast<float>(std::chrono::duration<double, std::milli>(end - start).count());
    };
    const auto integrateStart = std::chrono::steady_clock::now();

    m_lastGiStats = GiRuntimeStats{};
    const uint32_t cascadeCount = std::min(m_giConfig.cascadeCount, GiClipmapPlan::MAX_CASCADES);
    if (cascadeCount == 0) {
        return;
    }

    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        m_giCascadeRuntime[cascadeIndex].snappedOriginBlocks =
            GiClipmapPlan::snapCascadeOriginBlocks(probeAnchorPosition,
                                                   m_giConfig.cascades[cascadeIndex]);
    }

    const GiClipmapPlan::GiFrameBudget frameBudget =
        GiClipmapPlan::buildFrameBudget(m_giConfig, m_giCascadeRuntime, m_frameCounter);
    m_lastGiStats.probesUpdated = frameBudget.totalProbesUpdated;
    m_lastGiStats.raysCast = frameBudget.totalRaysCast;

    if (frameBudget.totalProbesUpdated > 0) {
        const bool dispatched = dispatchGiProbeCompute(chunkManager, frameBudget, cascadeCount);
        if (!dispatched) {
            m_lastGiStats.probesUpdated = 0;
            m_lastGiStats.raysCast = 0;
        }
    }

    const auto integrateEnd = std::chrono::steady_clock::now();
    m_lastGiStats.cpuIntegrateMs = measureMs(integrateStart, integrateEnd);
}

void VulkanRenderDevice::cleanupRemotePlayerAssets() {
    for (VkTexture &texture : m_remotePlayerTextures) {
        texture.cleanup();
    }
    m_remotePlayerTextures.clear();
    m_remotePlayerTextureViews.clear();
    if (m_remotePlayerModel) {
        m_remotePlayerModel->cleanupGpuResources();
        m_remotePlayerModel.reset();
    }
    m_remotePlayerAssetsLoaded = false;
}

void VulkanRenderDevice::initRayTracingScene() {
    if (!m_context || !m_context->isHardwareRayTracingSupported()) {
        resetRayTracingScene();
        return;
    }

    if (!m_rtSceneState) {
        m_rtSceneState = std::make_unique<RtSceneState>();
    }

    RtSceneState &rt = *m_rtSceneState;
    if (rt.commandPool == nullptr) {
        vk::CommandPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
        poolInfo.queueFamilyIndex = m_context->getGraphicsQueueFamily();
        rt.commandPool = vk::raii::CommandPool(m_context->getDevice(), poolInfo);
    }

    rt.ready = true;
    if (rt.chunkBlases.find(kRtDummyChunkPos) == rt.chunkBlases.end()) {
        CpuChunkMesh dummyMesh{};
        dummyMesh.vertices = {makePackedVoxelVertex(0u, 0u, 0u), makePackedVoxelVertex(1u, 0u, 0u),
                              makePackedVoxelVertex(0u, 1u, 0u)};
        dummyMesh.indices = {0u, 1u, 2u};
        dummyMesh.revision = 1u;
        (void)uploadChunkRayTracingGeometry(kRtDummyChunkPos, dummyMesh);
    }
    m_rtSceneDirty = true;
    (void)rebuildRayTracingScene();
}

void VulkanRenderDevice::collectRetiredRayTracingResources() {
    if (!m_rtSceneState) {
        return;
    }

    RtSceneState &rt = *m_rtSceneState;
    for (auto it = rt.retiredChunkBlases.begin(); it != rt.retiredChunkBlases.end();) {
        if (it->retireFrame > m_frameCounter) {
            ++it;
            continue;
        }
        it->blas.reset();
        it = rt.retiredChunkBlases.erase(it);
    }
    for (auto it = rt.retiredTlases.begin(); it != rt.retiredTlases.end();) {
        if (it->retireFrame > m_frameCounter) {
            ++it;
            continue;
        }
        it->reset();
        it = rt.retiredTlases.erase(it);
    }
}

void VulkanRenderDevice::resetRayTracingScene() {
    m_activeGiSceneTlas = VK_NULL_HANDLE;
    m_rtSceneDirty = false;
    if (!m_rtSceneState) {
        return;
    }
    m_rtSceneState->reset();
    m_rtSceneState.reset();
}

bool VulkanRenderDevice::uploadChunkRayTracingGeometry(const glm::ivec3 &chunkPos,
                                                       const CpuChunkMesh &cpuMesh) {
    if (!m_rtSceneState || !m_rtSceneState->ready || !m_context) {
        return false;
    }
    if (cpuMesh.vertices.empty() || cpuMesh.indices.empty()) {
        return false;
    }

    try {
        RtSceneState &rt = *m_rtSceneState;
        auto existingIt = rt.chunkBlases.find(chunkPos);
        if (existingIt != rt.chunkBlases.end() && existingIt->second.revision == cpuMesh.revision) {
            return true;
        }

        const vk::raii::Device &device = m_context->getDevice();
        const vk::raii::PhysicalDevice &physicalDevice = m_context->getPhysicalDevice();

        std::vector<glm::vec3> positions;
        positions.reserve(cpuMesh.vertices.size());
        for (const VoxelVertex &packed : cpuMesh.vertices) {
            positions.push_back(decodePackedVoxelPosition(packed));
        }

        std::vector<uint16_t> indices = cpuMesh.indices;
        if (positions.empty() || indices.empty()) {
            return false;
        }

        RtSceneState::ChunkBlas built{};
        const vk::DeviceSize vertexBytes =
            static_cast<vk::DeviceSize>(positions.size() * sizeof(glm::vec3));
        const vk::DeviceSize indexBytes =
            static_cast<vk::DeviceSize>(indices.size() * sizeof(uint16_t));
        createBufferWithAddressing(
            device, physicalDevice, vertexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            vk::MemoryPropertyFlagBits::eDeviceLocal, true, built.vertexBuffer);
        createBufferWithAddressing(
            device, physicalDevice, indexBytes,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            vk::MemoryPropertyFlagBits::eDeviceLocal, true, built.indexBuffer);

        std::vector<UploadContext::BufferCopyUpload> uploads;
        uploads.reserve(2);
        uploads.push_back(UploadContext::BufferCopyUpload{
            m_uploadContext.createStagingBuffer(physicalDevice, positions.data(), vertexBytes),
            *built.vertexBuffer.buffer, vertexBytes});
        uploads.push_back(UploadContext::BufferCopyUpload{
            m_uploadContext.createStagingBuffer(physicalDevice, indices.data(), indexBytes),
            *built.indexBuffer.buffer, indexBytes});
        m_uploadContext.submitCopyBufferBatch(std::move(uploads));
        m_uploadContext.waitIdle();

        const vk::DeviceAddress vertexAddress =
            getBufferDeviceAddress(device, *built.vertexBuffer.buffer);
        const vk::DeviceAddress indexAddress =
            getBufferDeviceAddress(device, *built.indexBuffer.buffer);
        const uint32_t primitiveCount = static_cast<uint32_t>(indices.size() / 3u);
        if (primitiveCount == 0u) {
            built.reset();
            return false;
        }

        vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
        triangles.vertexData.deviceAddress = vertexAddress;
        triangles.vertexStride = sizeof(glm::vec3);
        triangles.maxVertex = static_cast<uint32_t>(positions.size() - 1u);
        triangles.indexType = vk::IndexType::eUint16;
        triangles.indexData.deviceAddress = indexAddress;

        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
        geometry.flags = vk::GeometryFlagBitsKHR::eOpaque;
        geometry.geometry.triangles = triangles;

        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        const std::array<uint32_t, 1> primitiveCounts = {primitiveCount};
        const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo =
            device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, primitiveCounts);

        createBufferWithAddressing(device, physicalDevice, sizeInfo.accelerationStructureSize,
                                   vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, built.asBuffer);

        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = *built.asBuffer.buffer;
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        built.as = vk::raii::AccelerationStructureKHR(device, asCreateInfo);

        DeviceBufferAllocation scratchBuffer{};
        createBufferWithAddressing(device, physicalDevice, sizeInfo.buildScratchSize,
                                   vk::BufferUsageFlagBits::eStorageBuffer |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, scratchBuffer);
        buildInfo.dstAccelerationStructure = *built.as;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device, *scratchBuffer.buffer);

        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {&rangeInfo};
        const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {buildInfo};

        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
        VulkanUtils::endSingleTimeCommands(device, m_context->getGraphicsQueue(),
                                           std::move(commandBuffer));
        scratchBuffer.reset();

        built.revision = cpuMesh.revision;
        built.primitiveCount = primitiveCount;

        if (existingIt != rt.chunkBlases.end()) {
            RtSceneState::RetiredChunkBlas retired{};
            retired.blas = std::move(existingIt->second);
            retired.retireFrame = m_frameCounter + 24u;
            rt.retiredChunkBlases.push_back(std::move(retired));
            rt.chunkBlases.erase(existingIt);
        }

        rt.chunkBlases.insert_or_assign(chunkPos, std::move(built));
        m_rtSceneDirty = true;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to upload BLAS for chunk (" << chunkPos.x << ", "
                  << chunkPos.y << ", " << chunkPos.z << "): " << e.what() << "\n";
        return false;
    }
}

void VulkanRenderDevice::removeChunkRayTracingGeometry(const glm::ivec3 &chunkPos) {
    if (!m_rtSceneState) {
        return;
    }

    RtSceneState &rt = *m_rtSceneState;
    auto it = rt.chunkBlases.find(chunkPos);
    if (it == rt.chunkBlases.end()) {
        return;
    }

    RtSceneState::RetiredChunkBlas retired{};
    retired.blas = std::move(it->second);
    retired.retireFrame = m_frameCounter + 24u;
    rt.retiredChunkBlases.push_back(std::move(retired));
    rt.chunkBlases.erase(it);
    m_rtSceneDirty = true;
}

bool VulkanRenderDevice::rebuildRayTracingScene() {
    if (!m_rtSceneState || !m_rtSceneState->ready || !m_context) {
        m_activeGiSceneTlas = VK_NULL_HANDLE;
        m_rtSceneDirty = false;
        return false;
    }

    try {
        RtSceneState &rt = *m_rtSceneState;
        const vk::raii::Device &device = m_context->getDevice();
        const vk::raii::PhysicalDevice &physicalDevice = m_context->getPhysicalDevice();

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(rt.chunkBlases.size());
        for (const auto &[chunkPos, chunkBlas] : rt.chunkBlases) {
            if (chunkBlas.as == nullptr || chunkBlas.primitiveCount == 0u) {
                continue;
            }

            vk::AccelerationStructureDeviceAddressInfoKHR addrInfo{};
            addrInfo.accelerationStructure = *chunkBlas.as;
            const vk::DeviceAddress blasAddress =
                device.getAccelerationStructureAddressKHR(addrInfo);
            if (blasAddress == 0) {
                continue;
            }

            VkAccelerationStructureInstanceKHR instance{};
            instance.transform = makeTranslationTransform(glm::vec3(chunkPos * CHUNK_SIZE));
            instance.instanceCustomIndex = 0u;
            instance.mask = 0xFFu;
            instance.instanceShaderBindingTableRecordOffset = 0u;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference = blasAddress;
            instances.push_back(instance);
        }

        DeviceBufferAllocation instanceBuffer{};
        const vk::DeviceSize instanceBytes = static_cast<vk::DeviceSize>(
            std::max<size_t>(1u, instances.size()) * sizeof(VkAccelerationStructureInstanceKHR));
        createBufferWithAddressing(
            device, physicalDevice, instanceBytes,
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            true, instanceBuffer);
        if (!instances.empty()) {
            void *mapped = instanceBuffer.memory.mapMemory(0, instanceBytes);
            std::memcpy(
                mapped, instances.data(),
                static_cast<size_t>(instances.size() * sizeof(VkAccelerationStructureInstanceKHR)));
            instanceBuffer.memory.unmapMemory();
        }

        vk::AccelerationStructureGeometryInstancesDataKHR instancesData{};
        instancesData.arrayOfPointers = VK_FALSE;
        instancesData.data.deviceAddress = getBufferDeviceAddress(device, *instanceBuffer.buffer);

        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.geometryType = vk::GeometryTypeKHR::eInstances;
        geometry.geometry.instances = instancesData;

        vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        const uint32_t primitiveCount = static_cast<uint32_t>(instances.size());
        const std::array<uint32_t, 1> primitiveCounts = {primitiveCount};
        const vk::AccelerationStructureBuildSizesInfoKHR sizeInfo =
            device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, primitiveCounts);

        DeviceBufferAllocation newTlasBuffer{};
        createBufferWithAddressing(device, physicalDevice, sizeInfo.accelerationStructureSize,
                                   vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, newTlasBuffer);

        vk::AccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.buffer = *newTlasBuffer.buffer;
        asCreateInfo.size = sizeInfo.accelerationStructureSize;
        asCreateInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        vk::raii::AccelerationStructureKHR newTlas(device, asCreateInfo);

        DeviceBufferAllocation scratchBuffer{};
        createBufferWithAddressing(device, physicalDevice, sizeInfo.buildScratchSize,
                                   vk::BufferUsageFlagBits::eStorageBuffer |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   vk::MemoryPropertyFlagBits::eDeviceLocal, true, scratchBuffer);

        buildInfo.dstAccelerationStructure = *newTlas;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device, *scratchBuffer.buffer);

        vk::AccelerationStructureBuildRangeInfoKHR rangeInfo{};
        rangeInfo.primitiveCount = primitiveCount;
        const std::array<vk::AccelerationStructureBuildRangeInfoKHR *, 1> rangeInfos = {&rangeInfo};
        const std::array<vk::AccelerationStructureBuildGeometryInfoKHR, 1> buildInfos = {buildInfo};

        vk::raii::CommandBuffer commandBuffer =
            VulkanUtils::beginSingleTimeCommands(device, rt.commandPool);
        commandBuffer.buildAccelerationStructuresKHR(buildInfos, rangeInfos);
        VulkanUtils::endSingleTimeCommands(device, m_context->getGraphicsQueue(),
                                           std::move(commandBuffer));
        scratchBuffer.reset();
        instanceBuffer.reset();

        if (rt.tlas != nullptr) {
            RtSceneState::RetiredTlas retired{};
            retired.as = std::move(rt.tlas);
            retired.asBuffer = std::move(rt.tlasAsBuffer);
            retired.retireFrame = m_frameCounter + 24u;
            rt.retiredTlases.push_back(std::move(retired));
        }

        rt.tlas = std::move(newTlas);
        rt.tlasAsBuffer = std::move(newTlasBuffer);
        m_activeGiSceneTlas = (rt.tlas != nullptr)
                                  ? static_cast<VkAccelerationStructureKHR>(
                                        static_cast<vk::AccelerationStructureKHR>(*rt.tlas))
                                  : VK_NULL_HANDLE;
        m_rtSceneDirty = false;
        return m_activeGiSceneTlas != VK_NULL_HANDLE;
    } catch (const std::exception &e) {
        std::cerr << "[Vulkan][RT] Failed to rebuild TLAS: " << e.what() << "\n";
        return false;
    }
}

void VulkanRenderDevice::syncChunkMeshes(ChunkManager &chunkManager,
                                         const glm::ivec3 &cullingChunk) {
    if (!m_context) {
        return;
    }

    static constexpr size_t kMaxChunkUploadsPerFrame = 8;

    const auto &cpuMeshes = chunkManager.getCpuChunkMeshes();
    std::vector<glm::ivec3> chunksToRemove;
    chunksToRemove.reserve(m_chunkMeshes.size());
    for (const auto &[chunkPos, _cached] : m_chunkMeshes) {
        if (cpuMeshes.find(chunkPos) == cpuMeshes.end()) {
            chunksToRemove.push_back(chunkPos);
        }
    }

    struct UploadCandidate {
        glm::ivec3 chunkPos{};
        int64_t dist2 = 0;
    };
    std::vector<UploadCandidate> candidates;
    candidates.reserve(kMaxChunkUploadsPerFrame);

    auto xzDistance2 = [&cullingChunk](const glm::ivec3 &chunkPos) -> int64_t {
        const glm::ivec3 d = chunkPos - cullingChunk;
        return static_cast<int64_t>(d.x) * static_cast<int64_t>(d.x) +
               static_cast<int64_t>(d.z) * static_cast<int64_t>(d.z);
    };

    auto farthestIt = [&candidates]() {
        return std::max_element(
            candidates.begin(), candidates.end(),
            [](const UploadCandidate &a, const UploadCandidate &b) { return a.dist2 < b.dist2; });
    };

    for (const auto &[chunkPos, cpu] : cpuMeshes) {
        if (cpu.vertices.empty() || cpu.indices.empty()) {
            continue;
        }

        const auto cacheIt = m_chunkMeshes.find(chunkPos);
        if (cacheIt != m_chunkMeshes.end() && cacheIt->second.revision == cpu.revision &&
            cacheIt->second.mesh.getIndexCount() > 0) {
            continue;
        }

        const int64_t dist2 = xzDistance2(chunkPos);
        if (candidates.size() < kMaxChunkUploadsPerFrame) {
            candidates.push_back(UploadCandidate{chunkPos, dist2});
            continue;
        }

        auto it = farthestIt();
        if (it != candidates.end() && dist2 < it->dist2) {
            *it = UploadCandidate{chunkPos, dist2};
        }
    }

    for (const glm::ivec3 &chunkPos : chunksToRemove) {
        auto it = m_chunkMeshes.find(chunkPos);
        if (it == m_chunkMeshes.end()) {
            continue;
        }
        removeChunkRayTracingGeometry(chunkPos);
        retireChunkMesh(std::move(it->second.mesh));
        m_chunkMeshes.erase(it);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const UploadCandidate &a, const UploadCandidate &b) { return a.dist2 < b.dist2; });

    for (const UploadCandidate &candidate : candidates) {
        const glm::ivec3 &chunkPos = candidate.chunkPos;
        const auto cpuIt = cpuMeshes.find(chunkPos);
        if (cpuIt == cpuMeshes.end()) {
            continue;
        }

        const CpuChunkMesh &cpu = cpuIt->second;
        if (cpu.vertices.empty() || cpu.indices.empty()) {
            continue;
        }

        VulkanChunkMesh &cache = m_chunkMeshes[chunkPos];
        retireChunkMesh(std::move(cache.mesh));
        cache.mesh = VkMesh{};

        std::vector<VkMesh::PackedVoxelVertex> vertices;
        vertices.reserve(cpu.vertices.size());
        for (const VoxelVertex &packed : cpu.vertices) {
            VkMesh::PackedVoxelVertex out{};
            out.low = packed.low;
            out.high = packed.high;
            vertices.push_back(out);
        }
        std::vector<uint16_t> indices = cpu.indices;
        cache.mesh.setPackedVoxelGeometry(std::move(vertices), std::move(indices));
        cache.mesh.init(m_context->getDevice(), m_context->getPhysicalDevice(), m_uploadContext);
        cache.revision = cpu.revision;
        if (m_context->isHardwareRayTracingSupported()) {
            (void)uploadChunkRayTracingGeometry(chunkPos, cpu);
        }
    }
}

void VulkanRenderDevice::retireChunkMesh(VkMesh &&mesh) {
    static constexpr uint64_t kRetireDelayFrames = 24;
    RetiredChunkMesh retired{};
    retired.mesh = std::move(mesh);
    retired.retireFrame = m_frameCounter + kRetireDelayFrames;
    m_retiredChunkMeshes.push_back(std::move(retired));
}

void VulkanRenderDevice::collectRetiredChunkMeshes() {
    for (auto it = m_retiredChunkMeshes.begin(); it != m_retiredChunkMeshes.end();) {
        if (it->retireFrame > m_frameCounter) {
            ++it;
            continue;
        }
        it->mesh.cleanup();
        it = m_retiredChunkMeshes.erase(it);
    }
}

void VulkanRenderDevice::cleanupChunkMeshes() {
    for (auto &[_, mesh] : m_chunkMeshes) {
        mesh.mesh.cleanup();
    }
    m_chunkMeshes.clear();

    for (auto &retired : m_retiredChunkMeshes) {
        retired.mesh.cleanup();
    }
    m_retiredChunkMeshes.clear();

    if (m_rtSceneState) {
        m_rtSceneState->reset();
        m_activeGiSceneTlas = VK_NULL_HANDLE;
        m_rtSceneDirty = false;
    }
}
