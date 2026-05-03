#pragma once

#include "Shader.hpp"
#include "../IWorldItemRenderer.hpp"

#include <memory>

class Camera;
struct Runtime;

class WorldItemRenderer final : public IWorldItemRenderer {
public:
    void render(const Runtime &runtime, const Camera &activeCamera) override;
    void shutdown() override;

private:
    bool ensureDebugShaderLoaded();
    void ensureCubeMesh();

    unsigned int m_worldItemVao = 0;
    unsigned int m_worldItemVbo = 0;
    std::unique_ptr<Shader> m_debugShader;
};
