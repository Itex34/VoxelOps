#pragma once

#include <memory>
#include <string_view>
#include "RenderApi.hpp"
#include <SDL3/SDL.h>

class IRenderDevice;
class IGunRenderer;
class IGunSceneRenderer;
class IWorldItemRenderer;

struct RenderBackendWindowContext {
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;
};

std::unique_ptr<IRenderDevice> CreateRenderDevice(RenderApi api);
std::shared_ptr<IGunRenderer> CreateGunRenderer(RenderApi api);
std::unique_ptr<IGunSceneRenderer> CreateGunSceneRenderer(RenderApi api);
std::unique_ptr<IWorldItemRenderer> CreateWorldItemRenderer(RenderApi api);
RenderApi ResolveRenderApiFromEnvironment() noexcept;
bool CreateRenderBackendWindowContext(
    RenderApi api, const char *title, int width, int height, int swapInterval,
    RenderBackendWindowContext &outContext
);
std::string_view GetRenderApiName(RenderApi api) noexcept;
bool RenderApiUsesVulkanWindow(RenderApi api) noexcept;
bool RenderApiRequiresOpenGlContext(RenderApi api) noexcept;
