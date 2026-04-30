#include "App.hpp"

void App::renderWorldItems(Runtime &runtime, const Camera &activeCamera) {
    m_worldItemRenderer.render(runtime, activeCamera);
}
