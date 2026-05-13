#include "RenderDeviceFactory.hpp"

#include "IRenderDevice.hpp"
#include "IGunRenderer.hpp"
#include "IGunSceneRenderer.hpp"
#include "IWorldItemRenderer.hpp"
#include "OpenGL/OpenGLRenderDevice.hpp"
#include "OpenGL/OpenGLGunRenderer.hpp"
#include "OpenGL/OpenGLGunSceneRenderer.hpp"
#include "OpenGL/WorldItemRenderer.hpp"
#include "Vulkan/VulkanGunSceneRenderer.hpp"
#include "VulkanRenderDevice.hpp"

#include <glad/glad.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
    class NullWorldItemRenderer final : public IWorldItemRenderer {
    public:
        void render(const Runtime &, const Camera &) override {}
        void shutdown() override {}
    };

    SDL_Window *CreateOpenGlWindowForVersion(
        const char *title, int width, int height, int major, int minor
    ) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        return SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    }

    std::string TrimAscii(std::string_view value) {
        size_t first = 0;
        while (first < value.size() && static_cast<unsigned char>(value[first]) <= 0x20u) {
            ++first;
        }
        size_t last = value.size();
        while (last > first && static_cast<unsigned char>(value[last - 1]) <= 0x20u) {
            --last;
        }
        return std::string(value.substr(first, last - first));
    }
} // namespace

RenderApi ResolveRenderApiFromEnvironment() noexcept {
    const char *renderApiEnv = std::getenv("VOXELOPS_RENDER_API");
    if (renderApiEnv == nullptr) {
        return RenderApi::Vulkan;
    }

    std::string apiName = TrimAscii(std::string_view(renderApiEnv));
    for (char &c : apiName) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    if (apiName == "vulkan" || apiName == "vk") {
        return RenderApi::Vulkan;
    }
    if (apiName == "opengl" || apiName == "gl") {
        return RenderApi::OpenGL;
    }
    return RenderApi::OpenGL;
}

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

std::unique_ptr<IWorldItemRenderer> CreateWorldItemRenderer(RenderApi api) {
    switch (api) {
    case RenderApi::Vulkan:
        return std::make_unique<NullWorldItemRenderer>();
    case RenderApi::OpenGL:
    default:
        return std::make_unique<WorldItemRenderer>();
    }
}

bool CreateRenderBackendWindowContext(
    RenderApi api, const char *title, int width, int height, int swapInterval,
    RenderBackendWindowContext &outContext
) {
    outContext = {};

    if (RenderApiUsesVulkanWindow(api)) {
        outContext.window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (outContext.window == nullptr) {
            std::cerr << "SDL_CreateWindow (Vulkan) failed: " << SDL_GetError() << "\n";
            return false;
        }
        return true;
    }

    if (!RenderApiRequiresOpenGlContext(api)) {
        std::cerr << "Unsupported render API for window/context bootstrap.\n";
        return false;
    }

    outContext.window = CreateOpenGlWindowForVersion(title, width, height, 4, 3);
    if (outContext.window == nullptr) {
        std::cerr << "OpenGL 4.3 context creation failed, retrying with OpenGL 3.3.\n";
        outContext.window = CreateOpenGlWindowForVersion(title, width, height, 3, 3);
    }
    if (outContext.window == nullptr) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    outContext.glContext = SDL_GL_CreateContext(outContext.window);
    if (outContext.glContext == nullptr) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(outContext.window);
        outContext.window = nullptr;
        return false;
    }

    if (!SDL_GL_MakeCurrent(outContext.window, outContext.glContext)) {
        std::cerr << "SDL_GL_MakeCurrent failed: " << SDL_GetError() << "\n";
        SDL_GL_DestroyContext(outContext.glContext);
        outContext.glContext = nullptr;
        SDL_DestroyWindow(outContext.window);
        outContext.window = nullptr;
        return false;
    }

    if (!SDL_GL_SetSwapInterval(swapInterval)) {
        std::cerr << "SDL_GL_SetSwapInterval(" << swapInterval << ") failed: " << SDL_GetError()
                  << "\n";
    } else {
        std::cout << "[App] OpenGL swap interval: " << swapInterval << "\n";
    }

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
        std::cerr << "Failed to initialize GLAD.\n";
        SDL_GL_DestroyContext(outContext.glContext);
        outContext.glContext = nullptr;
        SDL_DestroyWindow(outContext.window);
        outContext.window = nullptr;
        return false;
    }

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << "\n";
    std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n";
    return true;
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
