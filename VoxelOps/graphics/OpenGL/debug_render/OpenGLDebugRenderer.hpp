#pragma once
#include "../Shader.hpp"


#include <glm/fwd.hpp>
#include <cstdint>
#include <memory>


enum class LineDrawMode : uint8_t{ TwoVertices, TriStrip, COUNT };

class Camera;

class OpenGLDebugRenderer {


public:

    // if LineDrawMode is set to TwoVertices, thickness will be ignored
    void drawLine(
        const glm::vec3 &p1,
        const glm::vec3 &p2,
        const glm::vec3 &color,
        LineDrawMode drawMode,
        float thickness,
        const Camera &activeCamera
    );

private:

    bool ensureDebugShaderLoaded();
    void ensureLineMesh(const glm::vec3 &p1, const glm::vec3 &p2, LineDrawMode drawMode, float thickness);
    
    unsigned int m_debugRendererVao = 0;
    unsigned int m_debugRendererVbo = 0;

    std::unique_ptr<Shader> m_debugShader;
};