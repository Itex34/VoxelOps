#include <glad/glad.h>
#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"
#include "../graphics/RealisticSkyBackend.hpp"
#include "../graphics/ShaderSkyBackend.hpp"
#include "../graphics/VulkanRenderDevice.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <glm/glm.hpp>

using namespace AppHelpers;

namespace {
constexpr SkyAtmospherePreset kDefaultRealisticAtmospherePreset = SkyAtmospherePreset::Hazy;
const glm::vec3 kDefaultSunDirection = glm::normalize(glm::vec3(0.0f, 0.43496552f, 0.90044713f));

std::unique_ptr<ISkyBackend> createSkyBackendForTier(
    GraphicsBackend backendTier,
    const std::string& skyVertPath,
    const std::string& skyFragPath)
{
    switch (backendTier) {
    case GraphicsBackend::Realistic:
        std::cout << "[App] Sky backend: realistic PBR sky.\n";
        return std::make_unique<RealisticSkyBackend>();
    case GraphicsBackend::Performance:
        std::cout << "[App] Sky backend: shader sky (performance).\n";
        return std::make_unique<ShaderSkyBackend>(skyVertPath, skyFragPath);
    case GraphicsBackend::Potato:
    default:
        std::cout << "[App] Sky backend: shader sky (potato fallback).\n";
        return std::make_unique<ShaderSkyBackend>(skyVertPath, skyFragPath);
    }
}
}

void App::configureBackendPolicy(Runtime& runtime) {
    const GraphicsBackend backendTier = runtime.renderer->getActiveBackend();

    runtime.supportsGL43Shaders =
        (runtime.renderer->getOpenGLVersionMajor() > 4) ||
        (runtime.renderer->getOpenGLVersionMajor() == 4 && runtime.renderer->getOpenGLVersionMinor() >= 3);

    switch (backendTier) {
    case GraphicsBackend::Realistic:
        runtime.chunkManager->enableAO = true;
        runtime.chunkManager->enableShadows = true;
        break;
    case GraphicsBackend::Performance:
        runtime.chunkManager->enableAO = true;
        runtime.chunkManager->enableShadows = false;
        break;
    case GraphicsBackend::Potato:
    default:
        runtime.chunkManager->enableAO = false;
        runtime.chunkManager->enableShadows = false;
        break;
    }

    std::cout
        << "[App] Render API: " << runtime.renderer->getApiName()
        << " | Backend tier: " << runtime.renderer->getActiveBackendName()
        << " | MDI usable: " << (runtime.renderer->isMDIUsable() ? "yes" : "no")
        << " | AO: " << (runtime.chunkManager->enableAO ? "on" : "off")
        << " | Shadows: " << (runtime.chunkManager->enableShadows ? "on" : "off")
        << " | Chunk shader profile: " << (runtime.supportsGL43Shaders ? "GL43" : "GL33")
        << "\n";
}


void App::initGameplay(Runtime& runtime) {
    runtime.chunkManager = std::make_unique<ChunkManager>(*runtime.renderer);
    configureBackendPolicy(runtime);

    const std::string playerModelPath =
        Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();

    runtime.player = std::make_unique<Player>(
        glm::vec3(0.0f, 60.0f, 0.0f),
        *runtime.chunkManager,
        playerModelPath
    );
    runtime.interpolatedPlayerCamera = runtime.player->getCamera();
    runtime.inputCallbacks = std::make_unique<InputCallbacks>(*runtime.player);
    if (m_RenderApi == RenderApi::OpenGL) {
        preloadGuns(runtime);
        (void)equipGun(runtime, kDefaultGunType);
    }
    else {
        runtime.preloadedGuns.clear();
        runtime.equippedGun = nullptr;
    }
}


void App::preloadGuns(Runtime& runtime) {
    runtime.preloadedGuns.clear();
    runtime.equippedGun = nullptr;

    for (const GunDefinition& definition : GetGunDefinitions()) {
        const uint16_t weaponId = ToWeaponId(definition.type);
        runtime.preloadedGuns[weaponId] = BuildGunFromDefinition(definition);
        std::cout
            << "[gun] preloaded " << definition.displayName
            << " (weaponId=" << weaponId << ")"
            << " model=" << ResolveGunModelPath(definition)
            << "\n";
    }
}


bool App::equipGun(Runtime& runtime, GunType gunType) {
    if (m_RenderApi == RenderApi::Vulkan) {
        runtime.equippedGun = nullptr;
        return false;
    }

    const GunDefinition* definition = FindGunDefinition(gunType);
    if (definition == nullptr) {
        std::cerr << "[gun] missing definition for weapon id=" << ToWeaponId(gunType) << "\n";
        return false;
    }

    if (runtime.equippedGun && runtime.equippedGunType == gunType) {
        return true;
    }

    const uint16_t weaponId = ToWeaponId(definition->type);
    auto it = runtime.preloadedGuns.find(weaponId);
    if (it == runtime.preloadedGuns.end()) {
        std::cerr
            << "[gun] preloaded entry missing for weaponId=" << weaponId
            << ", loading on-demand.\n";
        runtime.preloadedGuns[weaponId] = BuildGunFromDefinition(*definition);
        it = runtime.preloadedGuns.find(weaponId);
        if (it == runtime.preloadedGuns.end() || !it->second) {
            std::cerr << "[gun] unable to equip weaponId=" << weaponId << "\n";
            return false;
        }
    }

    runtime.equippedGun = it->second.get();
    runtime.equippedGunType = definition->type;
    runtime.shootSendInterval = std::max(0.03, static_cast<double>(definition->fireIntervalSeconds));
    runtime.equippedGunViewOffset = definition->viewOffset;
    runtime.equippedGunViewScale = definition->viewScale;
    runtime.equippedGunViewEulerDeg = definition->viewEulerDeg;

    if (runtime.clientNet.IsConnected() && runtime.player) {
        const NetworkInputState& input = runtime.player->getNetworkInputState();
        PlayerInput packet;
        packet.inputTick = runtime.inputTickCounter++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = weaponId;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (runtime.clientNet.SendPlayerInput(packet)) {
            Runtime::PendingInputEntry entry;
            entry.packet = packet;
            entry.deltaSeconds = Runtime::InputSendInterval;
            runtime.pendingInputs.push_back(entry);
            while (runtime.pendingInputs.size() > Runtime::MaxPendingInputs) {
                runtime.pendingInputs.pop_front();
            }
            runtime.lastInputSendTime = GetTimeSeconds();
        }
    }

    std::cout
        << "[gun] equipped " << definition->displayName
        << " (weaponId=" << weaponId << ")"
        << " [preloaded]"
        << "\n";
    return true;
}


void App::initCallbacks(Runtime& runtime) {
    runtime.callbackContext = CallbackContext{
        .inputCallbacks = runtime.inputCallbacks.get(),
        .skyBackend = runtime.sky.get(),
        .useDebugCamera = &m_UseDebugCamera
    };
    applyMouseInputModes();
}


void App::initRenderResources(Runtime& runtime) {
    if (m_RenderApi == RenderApi::Vulkan) {
        runtime.chunkShader.reset();
        runtime.dbgShader.reset();
        runtime.gunShader.reset();
        runtime.sky.reset();
        runtime.callbackContext.skyBackend = nullptr;
        runtime.chunkUniformsInitialized = false;
        runtime.supportsGL43Shaders = false;
        std::cout << "[App] Vulkan path: skipping OpenGL shader/sky initialization.\n";
        return;
    }

    const std::string chunkVertPath = runtime.supportsGL43Shaders
        ? Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack.vert").generic_string()
        : Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack33.vert").generic_string();
    const std::string chunkFragPath = runtime.supportsGL43Shaders
        ? Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack.frag").generic_string()
        : Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack33.frag").generic_string();
    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();
    const std::string skyVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/sky.vert").generic_string();
    const std::string skyFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/sky_simple.frag").generic_string();
    const std::string playerVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.vert").generic_string();
    const std::string playerFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.frag").generic_string();

    runtime.chunkShader = std::make_unique<Shader>(chunkVertPath.c_str(), chunkFragPath.c_str());
    runtime.dbgShader = std::make_unique<Shader>(
        debugVertPath.c_str(),
        debugFragPath.c_str()
    );
    runtime.gunShader = std::make_unique<Shader>(
        playerVertPath.c_str(),
        playerFragPath.c_str()
    );
    runtime.sky = createSkyBackendForTier(runtime.renderer->getActiveBackend(), skyVertPath, skyFragPath);
    runtime.sky->initialize();
    runtime.sky->setSunDir(kDefaultSunDirection);
    m_SunDirection = runtime.sky->getSunDir();
    m_SkyExposure = runtime.sky->getExposure();
    if (runtime.sky->supportsAtmospherePresets()) {
        runtime.sky->setAtmospherePreset(SkyAtmospherePreset::Clear);
    }
    runtime.sky->resize(GameData::screenWidth, GameData::screenHeight);
    runtime.callbackContext.skyBackend = runtime.sky.get();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

}


void App::initUi(Runtime& runtime) {
    if (m_RenderApi == RenderApi::Vulkan) {
        runtime.debugUi = std::make_unique<DebugUi>();
        runtime.inventoryUi = std::make_unique<InventoryUI>();

        bool initialized = false;
        if (auto* vulkanDevice = dynamic_cast<VulkanRenderDevice*>(runtime.renderer.get())) {
            UiVulkanInitInfo initInfo{};
            initInfo.instance = vulkanDevice->getVkInstanceHandle();
            initInfo.physicalDevice = vulkanDevice->getVkPhysicalDeviceHandle();
            initInfo.device = vulkanDevice->getVkDeviceHandle();
            initInfo.queueFamily = vulkanDevice->getVkGraphicsQueueFamily();
            initInfo.queue = vulkanDevice->getVkGraphicsQueueHandle();
            initInfo.renderPass = vulkanDevice->getVkRenderPassHandle();
            initInfo.imageCount = vulkanDevice->getVkSwapchainImageCount();
            initInfo.minImageCount = 2;
            initialized = runtime.debugUi->initializeForVulkan(m_Window, initInfo);
        }

        if (!initialized) {
            std::cerr << "[App] Failed to initialize ImGui Vulkan backend.\n";
            runtime.debugUi.reset();
            runtime.inventoryUi.reset();
            m_ShowDebugUi = false;
            m_ShowInventoryUi = false;
            m_ForceCursorEnabled = false;
            GameData::cursorEnabled = false;
            applyMouseInputModes();
            return;
        }

        runtime.debugUi->setVisible(m_ShowDebugUi);
        runtime.inventoryUi->setVisible(m_ShowInventoryUi);
        if (m_ShowDebugUi || m_ShowInventoryUi) {
            GameData::cursorEnabled = true;
        }
        std::cout << "[App] Vulkan path: ImGui/Inventory UI initialized.\n";
        applyMouseInputModes();
        return;
    }

    runtime.debugUi = std::make_unique<DebugUi>();
    if (!runtime.debugUi->initialize(m_Window, m_GlContext, "#version 330")) {
        std::cerr << "Failed to initialize ImGui debug UI.\n";
        runtime.debugUi.reset();
        return;
    }

    runtime.inventoryUi = std::make_unique<InventoryUI>();
    runtime.debugUi->setVisible(m_ShowDebugUi);
    runtime.inventoryUi->setVisible(m_ShowInventoryUi);
    if (m_ShowDebugUi || m_ShowInventoryUi) {
        GameData::cursorEnabled = true;
    }
    applyMouseInputModes();
}


void App::initNetworking(Runtime& runtime) {
    const double now = GetTimeSeconds();
    runtime.lastInputSendTime = now;
    runtime.lastChunkRequestSendTime = now;
    runtime.lastShootSendTime = now - runtime.shootSendInterval;
    runtime.nextReconnectAttemptTime = now;
    runtime.reconnectBackoffSeconds = 1.0;
    runtime.hasLastChunkRequestCenter = false;
    runtime.usernamePromptError.clear();
    runtime.pendingServerEndpointInput.fill('\0');
    runtime.pendingUsernameInput.fill('\0');
    {
        const std::string endpoint = m_ServerIp + ":" + std::to_string(m_ServerPort);
        const size_t copyLen = std::min(endpoint.size(), runtime.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        runtime.pendingServerEndpointInput[copyLen] = '\0';
    }
    if (!m_RequestedUsername.empty()) {
        const size_t copyLen = std::min(m_RequestedUsername.size(), runtime.pendingUsernameInput.size() - 1);
        std::memcpy(runtime.pendingUsernameInput.data(), m_RequestedUsername.data(), copyLen);
        runtime.pendingUsernameInput[copyLen] = '\0';
    }

    if (!runtime.clientNet.Start()) {
        std::cerr << "Failed to start networking\n";
        runtime.lastConnectionStatus = runtime.clientNet.GetConnectionStatusText();
        return;
    }

    runtime.lastConnectionStatus = runtime.clientNet.GetConnectionStatusText();
}


bool App::beginConnectionAttempt(Runtime& runtime) {
    if (!runtime.clientNet.ConnectTo(m_ServerIp, m_ServerPort)) {
        std::cerr << "ConnectTo(" << m_ServerIp << ":" << m_ServerPort << ") failed\n";
        return false;
    }
    if (!runtime.clientNet.SendConnectRequest(m_RequestedUsername)) {
        std::cerr << "Failed to send connect request\n";
        return false;
    }
    return true;
}

