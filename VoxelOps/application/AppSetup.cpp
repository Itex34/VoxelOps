#include "App.hpp"
#include "AppHelpers.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

using namespace AppHelpers;

void App::configureBackendPolicy(Runtime &runtime) {
    const RenderDeviceCapabilities caps = runtime.render.renderer->getCapabilities();

    runtime.gameplay.chunkManager->enableAO = true;
    runtime.gameplay.chunkManager->setMeshBakedLightingEnabled(caps.supportsBakedChunkLighting);

    std::cout << "[App] Render API: " << caps.apiName << " | Backend tier: " << caps.backendName
              << " | MDI usable: " << (caps.mdiUsable ? "yes" : "no")
              << " | AO: " << (runtime.gameplay.chunkManager->enableAO ? "on" : "off")
              << "\n";
}

void App::initGameplay(Runtime &runtime) {
    runtime.gameplay.chunkManager = std::make_unique<ChunkManager>();
    configureBackendPolicy(runtime);

    const std::string playerModelPath =
        Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();

    runtime.gameplay.player = std::make_unique<Player>(
        glm::vec3(0.0f, 60.0f, 0.0f), *runtime.gameplay.chunkManager, playerModelPath
    );
    runtime.render.interpolatedPlayerCamera = runtime.gameplay.player->getCamera();
    runtime.gameplay.inputCallbacks = std::make_unique<InputCallbacks>(*runtime.gameplay.player);
    runtime.combat.preloadedGuns.clear();
    runtime.combat.equippedGun = nullptr;
}

void App::preloadGuns(Runtime &runtime) {
    runtime.combat.preloadedGuns.clear();
    runtime.combat.equippedGun = nullptr;

    for (const GunDefinition &definition : GetGunDefinitions()) {
        const uint16_t weaponId = ToWeaponId(definition.type);
        runtime.combat.preloadedGuns[weaponId] = BuildGunFromDefinition(definition);
        if (runtime.render.gunRenderer) {
            const std::string modelPath = ResolveGunModelPath(definition);
            (void)runtime.render.gunRenderer->loadWeaponModel(weaponId, modelPath);
        }
        std::cout << "[gun] preloaded " << definition.displayName << " (weaponId=" << weaponId
                  << ")"
                  << " model=" << ResolveGunModelPath(definition) << "\n";
    }
}

bool App::equipGun(Runtime &runtime, GunType gunType) {
    if (!runtime.render.gunRenderer) {
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
        if (runtime.render.gunRenderer) {
            const std::string modelPath = ResolveGunModelPath(*definition);
            (void)runtime.render.gunRenderer->loadWeaponModel(weaponId, modelPath);
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

    if (runtime.network.clientNet.IsConnected() && runtime.gameplay.player) {
        const NetworkInputState &input = runtime.gameplay.player->getNetworkInputState();
        PlayerInput packet;
        packet.inputTick = runtime.prediction.inputTickCounter++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = weaponId;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (runtime.network.clientNet.SendPlayerInput(packet)) {
            RuntimePredictionState::PendingInputEntry entry;
            entry.packet = packet;
            entry.deltaSeconds = RuntimePredictionState::InputSendInterval;
            runtime.prediction.pendingInputs.push_back(entry);
            while (runtime.prediction.pendingInputs.size() > RuntimePredictionState::MaxPendingInputs) {
                runtime.prediction.pendingInputs.pop_front();
            }
            runtime.prediction.lastInputSendTime = GetTimeSeconds();
        }
    }

    std::cout << "[gun] equipped " << definition->displayName << " (weaponId=" << weaponId << ")"
              << " [preloaded]"
              << "\n";
    return true;
}

void App::initCallbacks(Runtime &) {
    applyMouseInputModes();
}

void App::initRenderResources(Runtime &runtime) {
    const RenderDeviceCapabilities caps = runtime.render.renderer->getCapabilities();
    runtime.render.gunRenderer = CreateGunRenderer(caps.api);
    runtime.render.gunSceneRenderer = CreateGunSceneRenderer(caps.api);
    runtime.combat.preloadedGuns.clear();
    runtime.combat.equippedGun = nullptr;

    if (!runtime.render.gunRenderer) {
        return;
    }

    if (!runtime.render.gunRenderer->initialize()) {
        runtime.render.gunRenderer.reset();
        return;
    }

    preloadGuns(runtime);
    (void)equipGun(runtime, kDefaultGunType);
}

void App::initUi(Runtime &runtime) {
    runtime.ui.debugUi = std::make_unique<DebugUi>();
    runtime.ui.inventoryUi = std::make_unique<InventoryUI>();

    if (!runtime.render.renderer->initializeDebugUi(
            *runtime.ui.debugUi, m_Window, reinterpret_cast<void *>(m_GlContext)
        )) {
        const RenderDeviceCapabilities caps = runtime.render.renderer->getCapabilities();
        std::cerr << "[App] Failed to initialize ImGui backend for " << caps.apiName << ".\n";
        runtime.ui.debugUi.reset();
        runtime.ui.inventoryUi.reset();
        m_ShowDebugUi = false;
        m_ShowInventoryUi = false;
        m_ForceCursorEnabled = false;
        GameData::cursorEnabled = false;
        applyMouseInputModes();
        return;
    }

    runtime.ui.debugUi->setVisible(m_ShowDebugUi);
    runtime.ui.inventoryUi->setVisible(m_ShowInventoryUi);
    if (m_ShowDebugUi || m_ShowInventoryUi) {
        GameData::cursorEnabled = true;
    }
    applyMouseInputModes();
}

void App::initNetworking(Runtime &runtime) {
    const double now = GetTimeSeconds();
    runtime.prediction.lastInputSendTime = now;
    runtime.world.lastChunkRequestSendTime = now;
    runtime.combat.lastShootSendTime = now - runtime.combat.shootSendInterval;
    runtime.app.connection.nextReconnectAttemptTime = now;
    runtime.app.connection.reconnectBackoffSeconds = 1.0;
    runtime.world.hasLastChunkRequestCenter = false;
    runtime.app.connection.usernamePromptError.clear();
    runtime.app.connection.pendingServerEndpointInput.fill('\0');
    runtime.app.connection.pendingUsernameInput.fill('\0');
    {
        const std::string endpoint = m_ServerIp + ":" + std::to_string(m_ServerPort);
        const size_t copyLen =
            std::min(endpoint.size(), runtime.app.connection.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.app.connection.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        runtime.app.connection.pendingServerEndpointInput[copyLen] = '\0';
    }
    if (!m_RequestedUsername.empty()) {
        const size_t copyLen = std::min(
            m_RequestedUsername.size(), runtime.app.connection.pendingUsernameInput.size() - 1
        );
        std::memcpy(
            runtime.app.connection.pendingUsernameInput.data(), m_RequestedUsername.data(), copyLen
        );
        runtime.app.connection.pendingUsernameInput[copyLen] = '\0';
    }

    if (!runtime.network.clientNet.Start()) {
        std::cerr << "Failed to start networking\n";
        runtime.app.connection.lastConnectionStatus = runtime.network.clientNet.GetConnectionStatusText();
        return;
    }

    runtime.app.connection.lastConnectionStatus = runtime.network.clientNet.GetConnectionStatusText();
}

bool App::beginConnectionAttempt(Runtime &runtime) {
    if (!runtime.network.clientNet.ConnectTo(m_ServerIp, m_ServerPort)) {
        std::cerr << "ConnectTo(" << m_ServerIp << ":" << m_ServerPort << ") failed\n";
        return false;
    }
    if (!runtime.network.clientNet.SendConnectRequest(m_RequestedUsername)) {
        std::cerr << "Failed to send connect request\n";
        return false;
    }
    return true;
}







