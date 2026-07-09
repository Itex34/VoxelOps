#include "App.hpp"
#include <utility>

App::App()
    : m_inputHost(*this)
    , m_connectionHost(*this)
    , m_windowHost(*this)
    , m_renderHost(*this) {}

FrameOrchestratorContext App::buildFrameOrchestratorContext() {
    FrameOrchestratorContext context{};
    context.inputHost = &m_inputHost;
    context.connectionHost = &m_connectionHost;
    context.windowHost = &m_windowHost;
    context.renderHost = &m_renderHost;
    context.host.window = m_Window;
    context.ui.serverIp = &m_ServerIp;
    context.ui.serverPort = &m_ServerPort;
    context.ui.requestedUsername = &m_RequestedUsername;
    context.simulation.useDebugCamera = &m_UseDebugCamera;
    context.render.toggleWireframe = &m_ToggleWireframe;
    context.render.toggleChunkBorders = &m_ToggleChunkBorders;
    context.render.toggleDebugFrustum = &m_ToggleDebugFrustum;
    context.ui.showDebugUi = &m_ShowDebugUi;
    context.ui.showInventoryUi = &m_ShowInventoryUi;
    context.ui.requestSwitchToOpenGl = &m_RequestSwitchToOpenGL;
    context.ui.requestSwitchToVulkan = &m_RequestSwitchToVulkan;
    context.ui.renderApiPreference = &m_RenderApiPreference;
    context.simulation.forceCursorEnabled = &m_ForceCursorEnabled;
    context.simulation.botMode = &m_BotMode;
    context.simulation.botSeed = &m_BotSeed;
    context.simulation.botShootRate = &m_BotShootRate;
    context.ui.enableRawMouseInput = &m_EnableRawMouseInput;
    context.render.skyExposure = &m_SkyExposure;
    context.render.sunDirection = &m_SunDirection;
    context.render.sunShadowDirectionalBias = &m_SunShadowDirectionalBias;
    context.render.sunShadowLowSunBiasBoost = &m_SunShadowLowSunBiasBoost;
    context.render.sunShadowFrontFaceCullAtLowSun = &m_SunShadowFrontFaceCullAtLowSun;
    context.render.sunShadowFrontFaceCullGrazingThreshold =
        &m_SunShadowFrontFaceCullGrazingThreshold;
    context.simulation.wasWorldInteractPressed = &m_WasWorldInteractPressed;

    return context;
}

void App::rebindFrameOrchestrator(Runtime &runtime) {
    m_frameOrchestrator.bind(runtime, buildFrameOrchestratorContext());
}

void App::renderWorldItems(Runtime &runtime, const Camera &activeCamera) {
    if (runtime.render.worldItemRenderer) {
        runtime.render.worldItemRenderer->render(runtime, activeCamera);
    }
}
