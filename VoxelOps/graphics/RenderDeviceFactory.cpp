#include "RenderDeviceFactory.hpp"

#include "IRenderDevice.hpp"
#include "IGunRenderer.hpp"
#include "IGunSceneRenderer.hpp"
#include "OpenGL/OpenGLRenderDevice.hpp"
#include "OpenGL/OpenGLGunRenderer.hpp"
#include "OpenGL/OpenGLGunSceneRenderer.hpp"
#include "Vulkan/VulkanGunSceneRenderer.hpp"
#include "VulkanRenderDevice.hpp"

std::unique_ptr<IRenderDevice> CreateRenderDevice(RenderApi api) {
    switch (api) {
    case RenderApi::Vulkan:
        return std::make_unique<VulkanRenderDevice>();
    case RenderApi::OpenGL:
    default:
        return std::make_unique<OpenGLRenderDevice>();
    }
}

std::shared_ptr<IGunRenderer> CreateGunRenderer(RenderApi api) {
    switch (api) {
    case RenderApi::Vulkan:
        return nullptr;
    case RenderApi::OpenGL:
    default:
        return std::make_shared<OpenGLGunRenderer>();
    }
}

std::unique_ptr<IGunSceneRenderer> CreateGunSceneRenderer(RenderApi api) {
    switch (api) {
    case RenderApi::Vulkan:
        return std::make_unique<VulkanGunSceneRenderer>();
    case RenderApi::OpenGL:
    default:
        return std::make_unique<OpenGLGunSceneRenderer>();
    }
}

std::string_view GetRenderApiName(RenderApi api) noexcept {
    switch (api) {
    case RenderApi::Vulkan:
        return "Vulkan";
    case RenderApi::OpenGL:
    default:
        return "OpenGL";
    }
}

bool RenderApiUsesVulkanWindow(RenderApi api) noexcept {
    return api == RenderApi::Vulkan;
}

bool RenderApiRequiresOpenGlContext(RenderApi api) noexcept {
    return api == RenderApi::OpenGL;
}
