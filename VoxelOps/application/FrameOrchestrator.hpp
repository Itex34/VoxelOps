#pragma once

#include "ClientHudSystem.hpp"
#include "ClientNetworkSystem.hpp"
#include "../runtime/Runtime.hpp"

#include <cstddef>

struct ImDrawData;

class App;

class FrameOrchestrator {
  public:
    FrameOrchestrator(App &app, Runtime &runtime);

    void runFrame();

  private:
    struct SimulationStageResult {
        size_t localPredictionSteps = 0;
        RenderDeviceCapabilities renderCaps{};
    };

    struct UiStageResult {
        ImDrawData *drawData = nullptr;
    };

    SimulationStageResult runSimulationStage();
    UiStageResult runUiStage(const SimulationStageResult &simulation);
    void runRenderStage(const SimulationStageResult &simulation, const UiStageResult &ui);
    void runPresentStage(size_t localPredictionSteps);
    void updateFrameTime();
    void updateFrameHotkeysAndCounters();

    App &m_app;
    Runtime &m_runtime;
    ClientNetworkSystem m_networkSystem;
    ClientHudSystem m_hudSystem;
    double m_frameNow = 0.0;
};
