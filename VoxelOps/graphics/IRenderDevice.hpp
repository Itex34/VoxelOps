#pragma once

#include <string_view>

#include "GraphicsBackend.hpp"
#include "RenderApi.hpp"
#include "../render/RenderScene.hpp"

struct SDL_Window;
class DebugUi;
struct UiFrameData;

struct RenderDeviceCapabilities {
    RenderApi api = RenderApi::OpenGL;
    std::string_view apiName = "OpenGL";
    GraphicsBackend backendTier = GraphicsBackend::Performance;
    std::string_view backendName = "Unknown";
    bool mdiUsable = false;
    bool supportsBakedChunkLighting = false;
    bool supportsGiRuntimeControls = false;
    bool supportsFirstPersonViewmodel = false;
    bool compositesUiInRenderFrame = false;
};

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual RenderDeviceCapabilities getCapabilities() const noexcept = 0;

    virtual bool initialize(SDL_Window *window) {
        (void)window;
        return true;
    }
    virtual void renderFrame(RenderScene &scene) = 0;
    virtual void onWindowResized(int width, int height) {
        (void)width;
        (void)height;
    }
    virtual bool initializeDebugUi(DebugUi &debugUi, SDL_Window *window, void *nativeContext) {
        (void)debugUi;
        (void)window;
        (void)nativeContext;
        return false;
    }
    virtual void appendBackendDebugUiFrameData(UiFrameData &frameData) const {
        (void)frameData;
    }
    virtual void present(SDL_Window *window) {
        (void)window;
    }
    virtual void shutdown() = 0;
};
