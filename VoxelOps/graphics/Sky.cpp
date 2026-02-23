#include "Sky.hpp"

#include "Shader.hpp"

#include <glm/gtc/matrix_inverse.hpp>

Sky::~Sky()
{
    shutdown();
}

void Sky::initialize(const char* vertexShaderPath, const char* fragmentShaderPath)
{
    shutdown();

    m_Shader = std::make_unique<Shader>(vertexShaderPath, fragmentShaderPath);

    const float skyVerts[] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f
    };

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyVerts), skyVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

void Sky::shutdown()
{
    if (m_VAO != 0) {
        glDeleteVertexArrays(1, &m_VAO);
        m_VAO = 0;
    }
    if (m_VBO != 0) {
        glDeleteBuffers(1, &m_VBO);
        m_VBO = 0;
    }
    m_Shader.reset();
}

void Sky::render(const glm::mat4& projection, const glm::mat4& view) const
{
    if (!m_Shader || m_VAO == 0) {
        return;
    }

    const glm::mat4 invSkyProj = glm::inverse(projection);
    const glm::mat4 invSkyView = glm::inverse(view);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    m_Shader->use();
    m_Shader->setMat4("uInvProj", invSkyProj);
    m_Shader->setMat4("uInvView", invSkyView);
    m_Shader->setVec3("uSunDir", m_SunDir);
    m_Shader->setFloat("uExposure", m_Exposure);
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void Sky::setSunDir(const glm::vec3& sunDir)
{
    m_SunDir = glm::normalize(sunDir);
}

const glm::vec3& Sky::getSunDir() const noexcept
{
    return m_SunDir;
}
