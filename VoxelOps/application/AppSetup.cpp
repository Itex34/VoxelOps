#include <glad/glad.h>
#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

using namespace AppHelpers;

void App::configureBackendPolicy(Runtime &runtime) {
    const RenderDeviceCapabilities caps = runtime.renderer->getCapabilities();

    runtime.supportsGL43Shaders = caps.supportsGL43Shaders;
    runtime.chunkManager->enableAO = true;
    runtime.chunkManager->enableShadows = false;
    runtime.chunkManager->setMeshBakedLightingEnabled(caps.supportsBakedChunkLighting);

    std::cout << "[App] Render API: " << caps.apiName << " | Backend tier: " << caps.backendName
              << " | MDI usable: " << (caps.mdiUsable ? "yes" : "no")
              << " | AO: " << (runtime.chunkManager->enableAO ? "on" : "off")
              << " | Shadows: " << (runtime.chunkManager->enableShadows ? "on" : "off")
              << " | Chunk shader profile: " << (runtime.supportsGL43Shaders ? "GL43" : "GL33")
              << "\n";
}

void App::initGameplay(Runtime &runtime) {
    runtime.chunkManager = std::make_unique<ChunkManager>();
    configureBackendPolicy(runtime);

    const std::string playerModelPath =
        Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();

    runtime.player = std::make_unique<Player>(glm::vec3(0.0f, 60.0f, 0.0f), *runtime.chunkManager,
                                              playerModelPath);
    runtime.interpolatedPlayerCamera = runtime.player->getCamera();
    runtime.inputCallbacks = std::make_unique<InputCallbacks>(*runtime.player);
    runtime.combat.preloadedGuns.clear();
    runtime.combat.equippedGun = nullptr;
}

void App::preloadGuns(Runtime &runtime) {
    runtime.combat.preloadedGuns.clear();
    runtime.combat.equippedGun = nullptr;

    for (const GunDefinition &definition : GetGunDefinitions()) {
        const uint16_t weaponId = ToWeaponId(definition.type);
        runtime.combat.preloadedGuns[weaponId] = BuildGunFromDefinition(definition);
        if (runtime.gunRenderer) {
            const std::string modelPath = ResolveGunModelPath(definition);
            (void)runtime.gunRenderer->loadWeaponModel(weaponId, modelPath);
        }
        std::cout << "[gun] preloaded " << definition.displayName << " (weaponId=" << weaponId
                  << ")"
                  << " model=" << ResolveGunModelPath(definition) << "\n";
    }
}

bool App::equipGun(Runtime &runtime, GunType gunType) {
    if (!runtime.gunRenderer) {
        runtime.combat.equippedGun = nullptr;
        return false;
    }

    const GunDefinition *definition = FindGunDefinition(gunType);
    if (definition == nullptr) {
        std::cerr << "[gun] missing definition for weapon id=" << ToWeaponId(gunType) << "\n";
        return false;
    }

    if (runtime.combat.equippedGun && runtime.combat.equippedGunType == gunType) {
        return true;
    }

    const uint16_t weaponId = ToWeaponId(definition->type);
    auto it = runtime.combat.preloadedGuns.find(weaponId);
    if (it == runtime.combat.preloadedGuns.end()) {
        std::cerr << "[gun] preloaded entry missing for weaponId=" << weaponId
                  << ", loading on-demand.\n";
        runtime.combat.preloadedGuns[weaponId] = BuildGunFromDefinition(*definition);
        if (runtime.gunRenderer) {
            const std::string modelPath = ResolveGunModelPath(*definition);
            (void)runtime.gunRenderer->loadWeaponModel(weaponId, modelPath);
        }
        it = runtime.combat.preloadedGuns.find(weaponId);
        if (it == runtime.combat.preloadedGuns.end() || !it->second) {
            std::cerr << "[gun] unable to equip weaponId=" << weaponId << "\n";
            return false;
        }
    }

    runtime.combat.equippedGun = it->second.get();
    runtime.combat.equippedGunType = definition->type;
    runtime.combat.shootSendInterval =
        std::max(0.03, static_cast<double>(definition->fireIntervalSeconds));
    runtime.combat.equippedGunViewOffset = definition->viewOffset;
    runtime.combat.equippedGunViewScale = definition->viewScale;
    runtime.combat.equippedGunViewEulerDeg = definition->viewEulerDeg;

    if (runtime.clientNet.IsConnected() && runtime.player) {
        const NetworkInputState &input = runtime.player->getNetworkInputState();
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

    std::cout << "[gun] equipped " << definition->displayName << " (weaponId=" << weaponId << ")"
              << " [preloaded]"
              << "\n";
    return true;
}

void App::initCallbacks(Runtime &runtime) {
    runtime.callbackContext = CallbackContext{.inputCallbacks = runtime.inputCallbacks.get(),
                                              .useDebugCamera = &m_UseDebugCamera};
    applyMouseInputModes();
}

void App::initRenderResources(Runtime &runtime) {
    const RenderDeviceCapabilities caps = runtime.renderer->getCapabilities();
    runtime.gunRenderer = CreateGunRenderer(caps.api);
    runtime.gunSceneRenderer = CreateGunSceneRenderer(caps.api);

    if (!caps.requiresOpenGlStateSetup) {
        runtime.dbgShader.reset();
        runtime.combat.preloadedGuns.clear();
        runtime.combat.equippedGun = nullptr;
        runtime.supportsGL43Shaders = false;
        return;
    }

    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();
    runtime.dbgShader = std::make_unique<Shader>(debugVertPath.c_str(), debugFragPath.c_str());
    if (!runtime.gunRenderer || !runtime.gunRenderer->initialize()) {
        runtime.gunRenderer.reset();
    } else {
        preloadGuns(runtime);
        (void)equipGun(runtime, kDefaultGunType);
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
}

void App::initUi(Runtime &runtime) {
    runtime.debugUi = std::make_unique<DebugUi>();
    runtime.inventoryUi = std::make_unique<InventoryUI>();

    if (!runtime.renderer->initializeDebugUi(*runtime.debugUi, m_Window,
                                             reinterpret_cast<void *>(m_GlContext))) {
        const RenderDeviceCapabilities caps = runtime.renderer->getCapabilities();
        std::cerr << "[App] Failed to initialize ImGui backend for " << caps.apiName << ".\n";
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
    applyMouseInputModes();
}

void App::initNetworking(Runtime &runtime) {
    const double now = GetTimeSeconds();
    runtime.lastInputSendTime = now;
    runtime.lastChunkRequestSendTime = now;
    runtime.combat.lastShootSendTime = now - runtime.combat.shootSendInterval;
    runtime.connection.nextReconnectAttemptTime = now;
    runtime.connection.reconnectBackoffSeconds = 1.0;
    runtime.hasLastChunkRequestCenter = false;
    runtime.connection.usernamePromptError.clear();
    runtime.connection.pendingServerEndpointInput.fill('\0');
    runtime.connection.pendingUsernameInput.fill('\0');
    {
        const std::string endpoint = m_ServerIp + ":" + std::to_string(m_ServerPort);
        const size_t copyLen =
            std::min(endpoint.size(), runtime.connection.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.connection.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        runtime.connection.pendingServerEndpointInput[copyLen] = '\0';
    }
    if (!m_RequestedUsername.empty()) {
        const size_t copyLen =
            std::min(m_RequestedUsername.size(), runtime.connection.pendingUsernameInput.size() - 1);
        std::memcpy(runtime.connection.pendingUsernameInput.data(), m_RequestedUsername.data(), copyLen);
        runtime.connection.pendingUsernameInput[copyLen] = '\0';
    }

    if (!runtime.clientNet.Start()) {
        std::cerr << "Failed to start networking\n";
        runtime.connection.lastConnectionStatus = runtime.clientNet.GetConnectionStatusText();
        return;
    }

    runtime.connection.lastConnectionStatus = runtime.clientNet.GetConnectionStatusText();
}

bool App::beginConnectionAttempt(Runtime &runtime) {
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
