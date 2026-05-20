#include <glad/glad.h>

#include "OpenGLDebugRenderer.hpp"
#include "../../../Shared/runtime/Paths.hpp"
#include "../../Camera.hpp"
#include "../../../data/GameData.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>


void OpenGLDebugRenderer::drawLine(
    const glm::vec3 &p1,
    const glm::vec3 &p2,
    const glm::vec3 &color,
    LineDrawMode drawMode,
    float thickness,
    const Camera &activeCamera
) {
    if (!ensureDebugShaderLoaded() || !m_debugShader) {
        return;
    }

        const float aspect =
        static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight);
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return;
    }

    ensureLineMesh(p1, p2, drawMode, thickness);

    if (m_debugRendererVao == 0 || m_debugRendererVbo == 0) {
        return;
    }

    const glm::mat4 projection = glm::perspective(
        glm::radians(GameData::FOV), aspect, GameData::nearPlane, GameData::farPlane
    );

    const glm::mat4 view = activeCamera.getViewMatrix();
    glm::mat4 model(1.0f);

    m_debugShader->use();
    m_debugShader->setMat4("view", view);
    m_debugShader->setMat4("projection", projection);
    m_debugShader->setMat4("model", model);
    m_debugShader->setVec3("color", color);

    const bool depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    GLint previousDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(static_cast<GLuint>(m_debugRendererVao));
    glDrawArrays(GL_LINES, 0, 2);
    glBindVertexArray(0);

    if (depthTestWasEnabled) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthFunc(static_cast<GLenum>(previousDepthFunc));

}



bool OpenGLDebugRenderer::ensureDebugShaderLoaded(){
    if (m_debugShader && glIsProgram(static_cast<GLuint>(m_debugShader->ID)) == GL_TRUE) {
        return true;
    }
    m_debugShader.reset();

    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();

    m_debugShader = std::make_unique<Shader>(debugVertPath.c_str(), debugFragPath.c_str());
    return m_debugShader != nullptr &&
           glIsProgram(static_cast<GLuint>(m_debugShader->ID)) == GL_TRUE;
}


void OpenGLDebugRenderer::ensureLineMesh(
    const glm::vec3 &p1, const glm::vec3 &p2, LineDrawMode drawMode, float thickness
) {
    if (drawMode != LineDrawMode::TwoVertices) {
        return;
    }

    (void)thickness;

    const float lineVertices[] = {
        p1.x, p1.y, p1.z,
        p2.x, p2.y, p2.z
    };

    const bool vaoValid =
        (m_debugRendererVao != 0) &&
        (glIsVertexArray(static_cast<GLuint>(m_debugRendererVao)) == GL_TRUE);
    const bool vboValid =
        (m_debugRendererVbo != 0) &&
        (glIsBuffer(static_cast<GLuint>(m_debugRendererVbo)) == GL_TRUE);

    if (!vaoValid || !vboValid) {
        const GLuint oldVao = static_cast<GLuint>(m_debugRendererVao);
        const GLuint oldVbo = static_cast<GLuint>(m_debugRendererVbo);
        if (oldVao != 0) {
            glDeleteVertexArrays(1, &oldVao);
        }
        if (oldVbo != 0) {
            glDeleteBuffers(1, &oldVbo);
        }
        m_debugRendererVao = 0;
        m_debugRendererVbo = 0;
    }

    if (m_debugRendererVao == 0 || m_debugRendererVbo == 0) {
        GLuint vao = 0;
        GLuint vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(lineVertices), lineVertices, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, reinterpret_cast<void *>(0)
        );
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        m_debugRendererVao = static_cast<unsigned int>(vao);
        m_debugRendererVbo = static_cast<unsigned int>(vbo);
    }
    else {
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_debugRendererVbo));
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(lineVertices), lineVertices);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}
