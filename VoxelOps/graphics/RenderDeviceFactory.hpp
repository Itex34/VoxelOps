#pragma once

#include <memory>

class IRenderDevice;

enum class RenderApi : unsigned char {
    OpenGL = 0,
    Vulkan = 1
};

std::unique_ptr<IRenderDevice> CreateRenderDevice(RenderApi api);
