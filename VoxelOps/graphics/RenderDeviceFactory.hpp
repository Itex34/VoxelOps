#pragma once

#include <memory>
#include <string_view>
#include "RenderApi.hpp"

class IRenderDevice;
class IGunRenderer;
class IGunSceneRenderer;

std::unique_ptr<IRenderDevice> CreateRenderDevice(RenderApi api);
std::shared_ptr<IGunRenderer> CreateGunRenderer(RenderApi api);
std::unique_ptr<IGunSceneRenderer> CreateGunSceneRenderer(RenderApi api);
std::string_view GetRenderApiName(RenderApi api) noexcept;
bool RenderApiUsesVulkanWindow(RenderApi api) noexcept;
bool RenderApiRequiresOpenGlContext(RenderApi api) noexcept;
