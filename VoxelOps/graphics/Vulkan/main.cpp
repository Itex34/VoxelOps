#include "graphics/Vulkan/core/Window.hpp"
#include "graphics/Vulkan/core/Player.hpp"
#include "graphics/Vulkan/graphics/Model.hpp"
#include "graphics/Vulkan/graphics/Texture.hpp"
#include "graphics/Vulkan/renderer/RenderFrameData.hpp"
#include "graphics/Vulkan/renderer/VulkanRenderer.hpp"
#include "graphics/Vulkan/vulkan/UploadContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanContext.hpp"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return -1;
    }

    try {
        Window window;
        window.init();

        VulkanContext context;
        context.init(window.getHandle());

        VulkanRenderer renderer(context);
        renderer.init();

        UploadContext assetUploadContext;
        assetUploadContext.init(context.getDevice(), context.getGraphicsQueueFamily(), context.getGraphicsQueue());

        VkModel playerModel;
        playerModel.loadModel("C:/Users/Sophie/source/repos/VoxelOps/Models/MinecraftPlayer/Player.fbx");
        playerModel.initGpuResources(context.getDevice(), context.getPhysicalDevice(), assetUploadContext);

        std::vector<VkTexture> playerTextures;
        const auto& meshTexturePaths = playerModel.getMeshTexturePaths();
        playerTextures.reserve(meshTexturePaths.size());
        for (const std::string& texturePath : meshTexturePaths) {
            VkTexture texture{};
            texture.initFromFile(
                context.getDevice(),
                context.getPhysicalDevice(),
                assetUploadContext,
                texturePath,
                context.isSamplerAnisotropyEnabled(),
                context.getMaxSamplerAnisotropy()
            );
            playerTextures.emplace_back(std::move(texture));
        }

        assetUploadContext.waitIdle();
        assetUploadContext.cleanup();

        std::vector<const VkTexture*> playerTextureViews;
        playerTextureViews.reserve(playerTextures.size());
        for (const VkTexture& texture : playerTextures) {
            playerTextureViews.emplace_back(&texture);
        }

        FrameRenderData frameRenderData;

        Player player(glm::vec3(0.0f, 1.7f, 3.0f));

        if (!SDL_SetWindowRelativeMouseMode(window.getHandle(), true)) {
            std::cerr << "Warning: could not enable relative mouse mode: " << SDL_GetError() << "\n";
        }

        bool running = true;
        while (running) {
            window.pollEvents(running);

            if (window.consumeMouseCaptureRequest() &&
                !SDL_GetWindowRelativeMouseMode(window.getHandle())) {
                if (!SDL_SetWindowRelativeMouseMode(window.getHandle(), true)) {
                    std::cerr << "Warning: could not re-enable relative mouse mode: " << SDL_GetError() << "\n";
                }
                else {
                    float discardX = 0.0f;
                    float discardY = 0.0f;
                    SDL_GetRelativeMouseState(&discardX, &discardY);
                }
            }

            player.update(window.getHandle());

            if (window.consumeResizeFlag() && !window.isMinimized()) {
                renderer.handleWindowResize(window.getWidth(), window.getHeight());
            }

            if (!window.isMinimized()) {
                frameRenderData.clear();
                RenderObject playerRenderObject{};
                playerRenderObject.model = &playerModel;
                playerRenderObject.meshTextures = &playerTextureViews;
                playerRenderObject.transform = glm::mat4(1.0f);
                frameRenderData.objects.push_back(playerRenderObject);

                const glm::mat4 projection =
                    player.getCamera().getProjectionMatrix(
                        static_cast<float>(window.getWidth()) / static_cast<float>(window.getHeight())
                    );
                const glm::mat4 view = player.getCamera().getViewMatrix();
                const glm::mat4 viewProjection = projection * view;

                renderer.renderFrame(
                    window.getWidth(),
                    window.getHeight(),
                    view,
                    projection,
                    viewProjection,
                    frameRenderData
                );
            }
        }

        renderer.cleanup();
        for (VkTexture& texture : playerTextures) {
            texture.cleanup();
        }
        playerModel.cleanupGpuResources();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        SDL_Quit();
        return -1;
    }

    SDL_Quit();
    return 0;
}



