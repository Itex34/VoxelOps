#include "App.hpp"

void App::renderWorldItems(Runtime &runtime, const Camera &activeCamera) {
    if (m_worldItemRenderer) {
        m_worldItemRenderer->render(runtime, activeCamera);
    }
}
