#include "RenderDeviceFactory.hpp"

#include "IRenderDevice.hpp"
#include "OpenGLRenderDevice.hpp"
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
