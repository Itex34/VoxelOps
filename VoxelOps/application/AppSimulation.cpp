#include "App.hpp"
#include "FrameOrchestrator.hpp"
#include "../graphics/IWorldItemRenderer.hpp"
#include <utility>

void App::processFrame(Runtime &runtime) {
    FrameOrchestratorContext context{};
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
    context.ui.enableRawMouseInput = &m_EnableRawMouseInput;
    context.render.skyExposure = &m_SkyExposure;
    context.render.sunDirection = &m_SunDirection;
    context.render.sunShadowDirectionalBias = &m_SunShadowDirectionalBias;
    context.render.sunShadowLowSunBiasBoost = &m_SunShadowLowSunBiasBoost;
    context.render.sunShadowFrontFaceCullAtLowSun = &m_SunShadowFrontFaceCullAtLowSun;
    context.render.sunShadowFrontFaceCullGrazingThreshold = &m_SunShadowFrontFaceCullGrazingThreshold;

    context.host.updateDebugCamera = [this](Runtime &ctxRuntime) { updateDebugCamera(ctxRuntime); };
    context.host.updateToggleStates = [this](Runtime &ctxRuntime) { updateToggleStates(ctxRuntime); };
    context.host.pollEvents = [this](Runtime &ctxRuntime) { pollEvents(ctxRuntime); };
    context.simulation.beginConnectionAttempt = [this](Runtime &ctxRuntime) {
        return beginConnectionAttempt(ctxRuntime);
    };
    context.simulation.equipGun = [this](Runtime &ctxRuntime, GunType gunType) {
        return equipGun(ctxRuntime, gunType);
    };
    context.render.renderWorldItems = [this](Runtime &ctxRuntime, const Camera &activeCamera) {
        m_worldItemRenderer->render(ctxRuntime, activeCamera);
    };
    context.host.applyMouseInputModes = [this]() { applyMouseInputModes(); };
    context.host.updateFPSCounter = [this]() { updateFPSCounter(); };
    context.host.toggleFullscreen = [this](SDL_Window *window) { toggleFullscreen(window); };
    context.simulation.wasWorldInteractPressed = &m_WasWorldInteractPressed;

    m_frameOrchestrator.bind(runtime, std::move(context));
    m_frameOrchestrator.runFrame();
}
