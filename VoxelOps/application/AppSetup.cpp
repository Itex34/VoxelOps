#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "App.hpp"
#include "AppHelpers.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using namespace AppHelpers;

void App::configureBackendPolicy(Runtime& runtime) {
    const Backend& backendInfo = runtime.renderer.getBackend();
    const GraphicsBackend backendTier = runtime.renderer.getActiveBackend();

    runtime.supportsGL43Shaders =
        (backendInfo.getOpenGLVersionMajor() > 4) ||
        (backendInfo.getOpenGLVersionMajor() == 4 && backendInfo.getOpenGLVersionMinor() >= 3);

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
        << "[App] Backend tier: " << runtime.renderer.getActiveBackendName()
        << " | MDI usable: " << (runtime.renderer.isMDIUsable() ? "yes" : "no")
        << " | AO: " << (runtime.chunkManager->enableAO ? "on" : "off")
        << " | Shadows: " << (runtime.chunkManager->enableShadows ? "on" : "off")
        << " | Chunk shader profile: " << (runtime.supportsGL43Shaders ? "GL43" : "GL33")
        << "\n";
}


void App::initGameplay(Runtime& runtime) {
    runtime.chunkManager = std::make_unique<ChunkManager>(runtime.renderer);
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
    preloadGuns(runtime);
    (void)equipGun(runtime, kDefaultGunType);
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
            runtime.lastInputSendTime = glfwGetTime();
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
        .useDebugCamera = &m_UseDebugCamera
    };

    glfwSetWindowUserPointer(m_Window, &runtime.callbackContext);
    glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* w, int width, int height) {
        auto* context = static_cast<CallbackContext*>(glfwGetWindowUserPointer(w));
        context->inputCallbacks->framebuffer_size_callback(w, width, height);
    });
    glfwSetCursorPosCallback(m_Window, [](GLFWwindow* w, double x, double y) {
        auto* context = static_cast<CallbackContext*>(glfwGetWindowUserPointer(w));
        context->inputCallbacks->mouse_callback(w, x, y, *context->useDebugCamera);
    });
    glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* w, int button, int action, int mods) {
        auto* context = static_cast<CallbackContext*>(glfwGetWindowUserPointer(w));
        context->inputCallbacks->mouse_button_callback(w, button, action, mods);
    });
    applyMouseInputModes();
}


void App::initRenderResources(Runtime& runtime) {
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
    runtime.sky.initialize(
        skyVertPath.c_str(),
        skyFragPath.c_str()
    );

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

}


void App::initUi(Runtime& runtime) {
    runtime.debugUi = std::make_unique<DebugUi>();
    if (!runtime.debugUi->initialize(m_Window, "#version 330")) {
        std::cerr << "Failed to initialize ImGui debug UI.\n";
        runtime.debugUi.reset();
        return;
    }

    runtime.debugUi->setVisible(m_ShowDebugUi);
    if (m_ShowDebugUi) {
        GameData::cursorEnabled = true;
    }
    applyMouseInputModes();
}


void App::initNetworking(Runtime& runtime) {
    const double now = glfwGetTime();
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

