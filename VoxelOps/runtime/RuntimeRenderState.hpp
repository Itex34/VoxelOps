#pragma once

#include "../graphics/Camera.hpp"
#include "../graphics/IGunRenderer.hpp"
#include "../graphics/IGunSceneRenderer.hpp"
#include "../graphics/IRenderDevice.hpp"
#include "../graphics/ISkyBackend.hpp"
#include "../graphics/IWorldItemRenderer.hpp"

#include <memory>

struct RuntimeRenderState {
    std::unique_ptr<IRenderDevice> renderer;
   
    std::unique_ptr<ISkyBackend> sky;
    std::shared_ptr<IGunRenderer> gunRenderer;
    std::unique_ptr<IGunSceneRenderer> gunSceneRenderer;
    std::unique_ptr<IWorldItemRenderer> worldItemRenderer;

    Camera debugCamera{glm::vec3(0.0f, 100.0f, 0.0f)};
    Camera interpolatedPlayerCamera{glm::vec3(0.0f)};
};
